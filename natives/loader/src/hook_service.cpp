#include <windows.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <MinHook.h>

#include <ykkz000/loader/internal.h>

// The process-wide single MinHook facade (hook service).
//
// Background: MinHook is a global singleton; if a plugin links its own copy of MinHook, the two
// sets of state corrupt each other's trampolines. Therefore all plugin hooks are installed through
// this service, registered for ownership by pluginHandle, and revoked by the Loader as a fallback
// on unload.
//
// Ownership policy:
//   - The Loader's own mechanism hooks (pluginHandle == nullptr): disabled but not removed; the
//     trampoline stays inside the (resident) Loader so the next game context can re-enable it.
//   - Plugin hooks: registered with pluginHandle; removeHooksForPlugin disables and removes them
//     when the plugin unloads, so the detour does not point at unloaded code.
//
// Idempotence contract (important): installHook / removeHook may be called repeatedly during
// "registration + every game-context creation". MinHook's MH_ERROR_ENABLED / MH_ERROR_DISABLED
// mean "the target is already in the desired state", which is success semantics and must never be
// reported as failure -- otherwise a plugin would treat an already-enabled hook as an install
// failure and clear the trampoline, leaving a silent-drop state that "still intercepts but no
// longer forwards". Reinstalling the same target must reuse the existing trampoline and write it
// back to the caller's original. The trampoline is stored in the record itself, so the reuse path
// does not depend on the plugin's original variable still holding it (a plugin may null its
// *Original between contexts).
namespace ykkz000::loader {
namespace {

struct HookRecord {
  void* detour = nullptr;
  void** original = nullptr;
  void* pluginHandle = nullptr;
  void* trampoline = nullptr; // service-owned; the trampoline survives a plugin clearing *original
};

std::mutex g_hookMutex;
std::unordered_map<void*, HookRecord> g_hooks;

// -- Call-time batch (fallback rollback on prepare failure) --
// registerEffectType opens a batch before calling the plugin prepare; hooks "newly created" within
// the batch are recorded; when prepare returns non-zero, endHookScope(true) removes every hook newly
// created in this batch in one shot. The primary responsibility remains the plugin revoking its own
// hooks when prepare fails; this batch is only a fallback keyed on "newly created during this call",
// and does not touch hooks that already existed before the batch was opened (the re-enable case is
// not covered here).
std::vector<std::vector<void*>> g_hookScopes;

std::once_flag g_initOnce;
bool g_initOk = false;

} // namespace

bool ensureHookServiceInitialized() {
  std::call_once(g_initOnce, []() {
    const MH_STATUS status = MH_Initialize();
    logInfoF("hook: MH_Initialize -> %d", static_cast<int>(status));
    g_initOk = (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED);
    if (!g_initOk) {
      logFatal("hook: MinHook initialization failed; all hooks unavailable");
    }
  });
  return g_initOk;
}

int installHookRaw(void* target, void* detour, void** original) {
  return serviceInstallHook(nullptr, target, detour, original);
}

int removeHookRaw(void* target) {
  return serviceRemoveHook(nullptr, target);
}

int serviceInstallHook(void* pluginHandle, void* target, void* detour, void** original) {
  if (target == nullptr || detour == nullptr || original == nullptr) {
    return -1;
  }
  if (!ensureHookServiceInitialized()) {
    return -2;
  }
  std::lock_guard<std::mutex> guard(g_hookMutex);
  const auto it = g_hooks.find(target);
  if (it == g_hooks.end()) {
    void* trampoline = nullptr;
    const MH_STATUS created = MH_CreateHook(target, detour, &trampoline);
    logInfoF("hook: create target=%p detour=%p -> %d trampoline=%p", target, detour,
             static_cast<int>(created), trampoline);
    if (created == MH_ERROR_ALREADY_CREATED) {
      // MinHook already has a hook for this target, but this service has no record: the existing
      // trampoline cannot be retrieved and the detour cannot be compared, so refuse to install as
      // a "different detour". Force-registering would yield the dangerous "enabled but trampoline
      // missing" state.
      logWarnF("hook: target=%p already created outside the hook service; refusing", target);
      return -3;
    }
    if (created != MH_OK) {
      return static_cast<int>(created);
    }
    HookRecord record;
    record.detour = detour;
    record.original = original;
    record.pluginHandle = pluginHandle;
    record.trampoline = trampoline;
    if (trampoline != nullptr) {
      *original = trampoline;
    }
    g_hooks.emplace(target, record);
    if (!g_hookScopes.empty()) {
      g_hookScopes.back().push_back(target); // for rollback when prepare fails
    }
  } else {
    // Already recorded: the same detour is treated as idempotent reuse; a different detour is
    // refused (keeping "one target, one detour").
    HookRecord& record = it->second;
    if (record.detour != detour) {
      logWarnF("hook: target=%p already hooked with a different detour; refusing", target);
      return -3;
    }
    if (record.trampoline != nullptr) {
      // Reuse the trampoline this service holds, independent of whether the plugin still keeps it:
      // a plugin may clear its *Original between contexts, and the caller must still receive a valid
      // trampoline so an already-installed hook is not mistaken for an install failure.
      *original = record.trampoline;
      logInfoF("hook: reuse target=%p trampoline=%p", target, record.trampoline);
    }
    if (pluginHandle != nullptr && record.pluginHandle == nullptr) {
      record.pluginHandle = pluginHandle; // upgrade ownership to the plugin
    }
  }
  const MH_STATUS enabled = MH_EnableHook(target);
  logInfoF("hook: enable target=%p -> %d%s", target, static_cast<int>(enabled),
           enabled == MH_ERROR_ENABLED ? " (already enabled)" : "");
  if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
    return static_cast<int>(enabled);
  }
  return 0;
}

int serviceRemoveHook(void* pluginHandle, void* target) {
  if (target == nullptr) {
    return -1;
  }
  std::lock_guard<std::mutex> guard(g_hookMutex);
  const auto it = g_hooks.find(target);
  if (it == g_hooks.end()) {
    return 0;
  }
  if (pluginHandle != nullptr && it->second.pluginHandle != nullptr &&
      it->second.pluginHandle != pluginHandle) {
    logWarnF("hook: target=%p belongs to another plugin; refusing removal", target);
    return -1;
  }
  const MH_STATUS disabled = MH_DisableHook(target);
  logInfoF("hook: disable target=%p -> %d%s", target, static_cast<int>(disabled),
           disabled == MH_ERROR_DISABLED ? " (already disabled)" : "");
  if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) {
    return static_cast<int>(disabled);
  }
  return 0;
}

void beginHookScope() {
  std::lock_guard<std::mutex> guard(g_hookMutex);
  g_hookScopes.emplace_back();
}

void endHookScope(bool rollback) {
  std::vector<void*> targets;
  {
    std::lock_guard<std::mutex> guard(g_hookMutex);
    if (g_hookScopes.empty()) {
      return;
    }
    targets = std::move(g_hookScopes.back());
    g_hookScopes.pop_back();
    if (!rollback) {
      return;
    }
    for (void* target : targets) {
      MH_DisableHook(target);
      MH_RemoveHook(target);
      g_hooks.erase(target);
    }
  }
  for (void* target : targets) {
    logInfoF("hook: scope rollback removed target=%p", target);
  }
}

void removeHooksForPlugin(void* pluginHandle) {
  if (pluginHandle == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_hookMutex);
  int removed = 0;
  for (auto it = g_hooks.begin(); it != g_hooks.end();) {
    if (it->second.pluginHandle != pluginHandle) {
      ++it;
      continue;
    }
    const void* target = it->first;
    MH_DisableHook(it->first);
    MH_RemoveHook(it->first);
    it = g_hooks.erase(it);
    ++removed;
    logInfoF("hook: removed plugin hook target=%p", target);
  }
  if (removed > 0) {
    logInfoF("hook: removed %d hook(s) for plugin %p", removed, pluginHandle);
  }
}

} // namespace ykkz000::loader
