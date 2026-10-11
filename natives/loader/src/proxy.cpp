#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <ykkz000/loader/internal.h>

#include "mods_sqlite.h"
#include "serialization_trace.h"

namespace ykkz000::loader {
namespace {

using CreateGameContextFn = void* (*)();
using DestroyGameContextFn = void (*)(void*);
using TelemetryHashFn = std::uint64_t (*)();

std::once_flag g_initOnce;
std::once_flag g_pluginPhaseOnce;
bool g_initialized = false;
CreateGameContextFn g_realCreate = nullptr;
DestroyGameContextFn g_realDestroy = nullptr;
TelemetryHashFn g_realTelemetryHash = nullptr;
bridge::Host g_host = {};

// -- Validated reads/writes exposed to plugins (bridge::Host services) --
int hostReadField(const void* base, std::size_t offset, std::size_t bytes, void* out) {
  if (base == nullptr || out == nullptr || bytes == 0) {
    return 0;
  }
  const auto* address = static_cast<const std::uint8_t*>(base) + offset;
  if (!isReadableRegion(address, bytes)) {
    return 0;
  }
  std::memcpy(out, address, bytes);
  return 1;
}

int hostWriteField(void* base, std::size_t offset, std::size_t bytes, const void* in) {
  if (base == nullptr || in == nullptr || bytes == 0) {
    return 0;
  }
  auto* address = static_cast<std::uint8_t*>(base) + offset;
  if (!isReadableRegion(address, bytes)) {
    return 0;
  }
  std::memcpy(address, in, bytes);
  return 1;
}

int hostIsCandidateObject(const void* pointer) {
  return isCandidateObject(pointer) ? 1 : 0;
}

int hostIsReadableRegion(const void* address, std::size_t bytes) {
  return isReadableRegion(address, bytes) ? 1 : 0;
}

bool initialize() {
  installCrashCapture();  // install as early as possible so any later crash is recorded
  LogScope scope("initialize");
  {
    LogScope s("resolve real GameCore");
    if (!ensureGameCoreLoaded()) {
      logFatal("Initialization failed: real GameCore unavailable");
      return false;
    }
  }

  const GameCoreApi& api = gameCore();
  {
    LogScope s("resolve GameCore exports");
    g_realCreate = reinterpret_cast<CreateGameContextFn>(
        GetProcAddress(api.module, "DllCreateGameContext"));
    g_realDestroy = reinterpret_cast<DestroyGameContextFn>(
        GetProcAddress(api.module, "DllDestroyGameContext"));
    g_realTelemetryHash = reinterpret_cast<TelemetryHashFn>(
        GetProcAddress(api.module, "EXP_GetTelemetrySessionHash"));
  }
  if (g_realCreate == nullptr) {
    logFatal("Initialization failed: could not resolve real DllCreateGameContext");
    return false;
  }

  // Effect handler registration: the hooks must be installed before the real DllCreateGameContext,
  // so that the built-in handler tables constructed while the game context is created are captured
  // and custom effects are re-registered.
  const bool handlerHookReady = installEffectHandlerHook();
  logInfoF("effect handler hook ready=%d", handlerHookReady ? 1 : 0);

  // Serialization read trace (diagnostics, _DEBUG only): install after the GameCore entry points
  // are resolved. The hooks are Loader-owned and never collide with plugin hooks.
  installSerializationTrace();

  {
    LogScope s("build host interface");
    g_host.apiVersion = bridge::kHostApiVersion;
    g_host.makeHash = &makeHash;
    g_host.registerEffectType = &registerEffectType;
    g_host.log = &logMessage;
    g_host.gameCoreModule = api.module;
    g_host.getEffectRegistry = reinterpret_cast<bridge::GetEffectRegistryFn>(api.getEffectRegistry);
    g_host.engine = &engineApi();
    g_host.installHook = &serviceInstallHook;
    g_host.removeHook = &serviceRemoveHook;
    g_host.readField = &hostReadField;
    g_host.writeField = &hostWriteField;
    g_host.isCandidateObject = &hostIsCandidateObject;
    g_host.isReadableRegion = &hostIsReadableRegion;
    g_host.onGameContext = nullptr; // registered by each plugin in its own Host copy
    g_host.pluginHandle = nullptr;  // filled in by each plugin's Host copy
  }

  return true;
}

// Mods.sqlite-derived enabled-mod source. The snapshot is cached together with the file identity
// (LastWriteTime plus size): the first read happens in the pre-create plugin phase, and a later
// context creation re-reads only when the engine rewrote the database. A snapshot that only becomes
// usable after the plugin phase can still seed the deferred cross-mod load.
struct ModsSqliteCache {
  bool have = false;    // a file identity is recorded
  bool ok = false;      // the last read produced a snapshot
  bool applied = false; // the snapshot has been pushed into the plugin manager
  std::uint64_t size = 0;
  FILETIME writeTime = {};
  ModsSqliteSnapshot snapshot;
};

ModsSqliteCache g_modsSqlite;

// Read Mods.sqlite unless the cached snapshot still matches the file identity.
bool readModsSqliteCached(ModsSqliteSnapshot& out) {
  const std::wstring path = modsSqlitePath();
  WIN32_FILE_ATTRIBUTE_DATA attributes = {};
  if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
    g_modsSqlite = ModsSqliteCache{}; // no database: there is no group-derived set
    return false;
  }
  const std::uint64_t size =
      (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
  if (g_modsSqlite.have && g_modsSqlite.size == size &&
      g_modsSqlite.writeTime.dwLowDateTime == attributes.ftLastWriteTime.dwLowDateTime &&
      g_modsSqlite.writeTime.dwHighDateTime == attributes.ftLastWriteTime.dwHighDateTime) {
    if (!g_modsSqlite.ok) {
      return false;
    }
    out = g_modsSqlite.snapshot;
    return true;
  }
  ModsSqliteSnapshot snapshot;
  const bool ok = readModsSqlite(snapshot); // the module logs its own failures
  g_modsSqlite.have = true;
  g_modsSqlite.ok = ok;
  g_modsSqlite.applied = false;
  g_modsSqlite.size = size;
  g_modsSqlite.writeTime = attributes.ftLastWriteTime;
  g_modsSqlite.snapshot = snapshot;
  if (!ok) {
    return false;
  }
  out = std::move(snapshot);
  return true;
}

// Push a snapshot into the plugin manager (once per read) and log its ids. Returns true when the
// snapshot carries a usable enabled-mod set for this call.
bool applyModsSqliteSnapshot(const ModsSqliteSnapshot& snapshot, bool attemptDeferred) {
  if (snapshot.enabledModIds.empty()) {
    logDebug("mods-sqlite: the selected group has no enabled members");
    return false;
  }
  if (g_modsSqlite.applied) {
    return true;
  }
  // The ids decide which mods' plugin directories are scanned; log them (debug build) so a missing
  // enabled mod can be told apart from a discovery or dependency failure.
  constexpr std::size_t kIdLogCap = 128;
  const std::size_t logged =
      snapshot.enabledModIds.size() < kIdLogCap ? snapshot.enabledModIds.size() : kIdLogCap;
  for (std::size_t i = 0; i < logged; ++i) {
    logDebugF("enabled-mods: sqlite id[%zu]=%s", i, snapshot.enabledModIds[i].c_str());
  }
  if (logged < snapshot.enabledModIds.size()) {
    logDebugF("enabled-mods: sqlite id log truncated (%zu of %zu)", logged,
              snapshot.enabledModIds.size());
  }
  setGroupEnabledModIds(snapshot.enabledModIds);
  setGroupEnabledModInfoPaths(snapshot.idToModinfoPath);
  g_modsSqlite.applied = true;
  if (attemptDeferred) {
    // The plugin phase already ran (this is a re-read in a later context): the newly seeded set can
    // still feed the deferred cross-mod load.
    (void)loadCrossModPluginsDeferred();
  }
  return true;
}

// Re-read after the plugin phase: the engine rewrites Mods.sqlite while it starts, so a rewrite (or
// a first read that failed) can still supply the group-derived set to the deferred cross-mod phase.
void refreshModsSqliteEnabledMods() {
  ModsSqliteSnapshot snapshot;
  if (!readModsSqliteCached(snapshot)) {
    return;
  }
  (void)applyModsSqliteSnapshot(snapshot, /*attemptDeferred=*/true);
}

// Pre-create plugin phase: resolve the enabled-mod set, then load the plugins (including the
// cross-mod directories that the set unlocks). Both steps must complete before the real
// DllCreateGameContext, which subsequently triggers ModifierLibrary::Initialize and
// DatabaseWriter::WriteEffectData: the plugins register their effect types here, and only a
// registration that precedes that build is accepted by the engine. The only enabled-mod source is
// the Mods.sqlite selected group; when it is unavailable, cross-mod discovery stays limited to the
// Loader's own plugin directories (the deferred phase can still pick up a later group-derived
// seed).
void loadPluginsBeforeCreate() {
  {
    LogScope s("resolve enabled mods");
    ModsSqliteSnapshot snapshot;
    if (readModsSqliteCached(snapshot) && applyModsSqliteSnapshot(snapshot, false)) {
      // The Mods.sqlite selected group seeded the set.
    } else {
      logInfo(
          "enabled-mods: no Mods.sqlite selected group available; cross-mod discovery stays "
          "limited to the Loader's own directory");
    }
  }
  {
    LogScope s("load plugins");
    loadPlugins(&g_host);
  }
}

} // namespace

void initializeLoaderOnce() {
  // Configure the log4cxx backend before any LogScope so the startup lines reach the log file.
  // logMessage() also initializes lazily, covering exported entry points whose LogScope runs
  // before this function (for example createGameContext).
  initializeLogging();
  LogScope scope("initialize loader");
  std::call_once(g_initOnce, []() { g_initialized = initialize(); });
}

bool loaderInitialized() {
  return g_initialized;
}

void* createGameContext() {
  LogScope scope("create game context");
  logDebugF("context: created t0");
  initializeLoaderOnce();
  // Re-assert the handler hooks (idempotent): the plugin phase below registers the effect types
  // whose handler capture depends on these hooks, and the real create must be preceded by them.
  (void)installEffectHandlerHook();
  if (g_realCreate == nullptr) {
    return nullptr;
  }
  // The plugin phase (Mods.sqlite selected-group read plus plugin load) runs once, at the first
  // context creation, so the plugins register before the real create builds and validates the
  // effect database. A later context cannot redo it: the engine validates the database only once
  // per process.
  std::call_once(g_pluginPhaseOnce, &loadPluginsBeforeCreate);
  // Mods.sqlite may have been rewritten (or become readable) since the plugin phase; a re-read can
  // still seed the deferred cross-mod load. No-op while the cached snapshot matches the file.
  refreshModsSqliteEnabledMods();
  // Notify plugins: a new context is being created. Plugins reinstall/re-enable their own hooks
  // here and clear caches that become invalid with the context. The notification follows the load
  // above, so plugins loaded in this call receive it for the current context.
  notifyPluginsGameContext(bridge::GameContextEvent::kCreated, nullptr);
  LogScope call("call real DllCreateGameContext");
  return g_realCreate();
}

void destroyGameContext(void* context) {
  LogScope scope("destroy game context");
  // Must precede the real destroy: the handler root is still valid here, so our registered nodes
  // can be safely detached; otherwise the engine shutdown phase would call a handler that no longer
  // exists and crash (see the exit-crash analysis).
  removeCustomEffectHandlers();
  if (g_realDestroy != nullptr) {
    LogScope call("call real DllDestroyGameContext");
    g_realDestroy(context);
  }
  // The context has been destroyed: notify plugins to disable hooks and clear caches.
  //
  // Note: the plugin DLLs stay loaded and are not unloaded here. The game repeatedly
  // creates/destroys contexts between the main menu and a match, whereas effect registration
  // happens only once (registration must precede ModifierLibrary::Initialize), so a plugin could
  // not re-register in the next context after being unloaded. Plugins live for the whole process;
  // when a real unload is needed (process exit), unloadPlugins() handles it in the order
  // "notify first, then fallback restore, then FreeLibrary".
  notifyPluginsGameContext(bridge::GameContextEvent::kDestroyed, context);
  uninstallEffectHandlerHook();
}

std::uint64_t telemetrySessionHash() {
  LogScope scope("telemetry session hash");
  initializeLoaderOnce();
  if (g_realTelemetryHash == nullptr && ensureGameCoreLoaded()) {
    g_realTelemetryHash = reinterpret_cast<TelemetryHashFn>(
        GetProcAddress(gameCore().module, "EXP_GetTelemetrySessionHash"));
  }
  return g_realTelemetryHash != nullptr ? g_realTelemetryHash() : 0;
}

} // namespace ykkz000::loader

extern "C" void* DllCreateGameContext() {
  return ykkz000::loader::createGameContext();
}

extern "C" void DllDestroyGameContext(void* context) {
  ykkz000::loader::destroyGameContext(context);
}

extern "C" int InitLoader() {
  ykkz000::loader::initializeLoaderOnce();
  return ykkz000::loader::loaderInitialized() ? 1 : 0;
}

extern "C" std::uint64_t EXP_GetTelemetrySessionHash() {
  return ykkz000::loader::telemetrySessionHash();
}
