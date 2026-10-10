#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ykkz000/loader/internal.h>

// Effect "mechanism" layer: generalized custom EffectType implementation registration + factory
// Create wrapping + effect-object vtable slot replacement and ownership registration.
//
// This file knows no concrete behavior: behavior is supplied by plugins via bridge::EffectImpl
// (Apply/Remove/analyze/handlerApply function pointers); the Loader only locates and replaces the
// corresponding slots in the cloned vtable by function-pointer value. It never modifies the
// engine's shared static vtable.
namespace ykkz000::loader {
namespace {

// MSVC x64 hidden return: the shared_ptr return is passed via RDX, and the function must return the
// same pointer in RAX.
using FactoryCreateFn = void* (*)(void* self, void* outSharedPtr, const void* params);

std::mutex g_recordMutex;
std::unordered_map<std::uint32_t, EffectRecord> g_records;

// Vtable block cloned by the Loader and installed on the effect object. When a plugin is unloaded,
// the slots the plugin replaced must be restored to the template original functions; otherwise a
// later engine call jumps into unloaded memory (the highest-risk point).
struct CloneRecord {
  void** block = nullptr;                                    // block start (including the RTTI prefix slot)
  void** clone = nullptr;                                    // vptr handed back to the engine
  void* pluginHandle = nullptr;
  std::vector<std::pair<std::size_t, const void*>> replaced; // slot index -> template function
};

std::mutex g_cloneMutex;
std::vector<CloneRecord> g_clones;

#if defined(_DEBUG)
// Diagnostic counter: confirms whether the clone list grows without bound over a long session (the
// main suspect for memory pressure that slows down or crashes a load).
std::atomic<long> g_patchCalls{0};

// Copies the clone count under its own lock; the caller logs after the lock is released.
std::size_t cloneCount() {
  std::lock_guard<std::mutex> guard(g_cloneMutex);
  return g_clones.size();
}
#endif

} // namespace

bool findEffectRecord(std::uint32_t typeHash, EffectRecord& out) {
  std::lock_guard<std::mutex> guard(g_recordMutex);
  const auto it = g_records.find(typeHash);
  if (it == g_records.end()) {
    return false;
  }
  out = it->second;
  return true;
}

int registerEffectImpl(std::uint32_t typeHash, const bridge::EffectImpl* impl,
                       void* originalCreate) {
  if (typeHash == 0 || impl == nullptr || originalCreate == nullptr) {
    return -1;
  }
  EffectRecord record;
  record.impl = *impl; // copy: still safe to read after the plugin is unloaded
  record.originalCreate = originalCreate;
  record.pluginHandle = activePluginHandle();
  std::lock_guard<std::mutex> guard(g_recordMutex);
  g_records.insert_or_assign(typeHash, record);
  logInfoF("custom: impl registered hash=0x%08X label=%s apply=%p remove=%p", typeHash,
           record.impl.label != nullptr ? record.impl.label : "(none)",
           reinterpret_cast<void*>(record.impl.apply),
           reinterpret_cast<void*>(record.impl.remove));
  return 0;
}

int unregisterEffectImpl(std::uint32_t typeHash) {
  if (typeHash == 0) {
    return -1;
  }
  std::lock_guard<std::mutex> guard(g_recordMutex);
  return g_records.erase(typeHash) > 0 ? 0 : -1;
}

void* patchEffectObjectSlots(void* effectObject, std::uint32_t typeHash) {
  if (effectObject == nullptr) {
    return nullptr;
  }
  EffectRecord record;
  if (!findEffectRecord(typeHash, record)) {
    logErrorF("Custom effect: missing implementation record hash=0x%08X", typeHash);
    return nullptr;
  }
  const bridge::EffectImpl& impl = record.impl;
  const bool wantApply = impl.apply != nullptr && impl.templateApply != nullptr;
  const bool wantRemove = impl.remove != nullptr && impl.templateRemove != nullptr;
  if (!wantApply && !wantRemove) {
    return nullptr; // fully reuse template behavior
  }

  auto* source = *reinterpret_cast<void***>(effectObject);
  if (source == nullptr) {
    return nullptr;
  }
  const std::size_t slots = kEffectVTableCloneSlots;
  auto* block = static_cast<void**>(
      HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * (slots + civ6::kVTableRttiPrefixSlots)));
  if (block == nullptr) {
    return nullptr;
  }
  block[0] = source[-1]; // MSVC's RTTI/COL pointer, must be preserved as well
  std::memcpy(block + civ6::kVTableRttiPrefixSlots, source, sizeof(void*) * slots);
  void** clone = block + civ6::kVTableRttiPrefixSlots;

  std::vector<std::pair<std::size_t, const void*>> replaced;
  bool applyPatched = !wantApply;
  bool removePatched = !wantRemove;
  std::size_t applySlot = 0;
  std::size_t removeSlot = 0;
  for (std::size_t i = 0; i < slots; ++i) {
    if (!applyPatched && source[i] == impl.templateApply) {
      clone[i] = reinterpret_cast<void*>(impl.apply);
      applyPatched = true;
      applySlot = i;
      replaced.emplace_back(i, impl.templateApply);
    } else if (!removePatched && source[i] == impl.templateRemove) {
      clone[i] = reinterpret_cast<void*>(impl.remove);
      removePatched = true;
      removeSlot = i;
      replaced.emplace_back(i, impl.templateRemove);
    }
  }
  if (!applyPatched || !removePatched) {
    logErrorF("Custom effect: Apply/Remove slot not matched in effect object vtable; "
              "keeping template behavior");
    HeapFree(GetProcessHeap(), 0, block);
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> guard(g_cloneMutex);
    CloneRecord cloneRecord;
    cloneRecord.block = block;
    cloneRecord.clone = clone;
    cloneRecord.pluginHandle = record.pluginHandle;
    cloneRecord.replaced = std::move(replaced);
    g_clones.push_back(std::move(cloneRecord));
  }
  logDebugF("custom: patched effect vtable hash=0x%08X label=%s block=%p vtable=%p "
            "applySlot=%zX removeSlot=%zX",
            typeHash, impl.label != nullptr ? impl.label : "(none)", block, clone, applySlot,
            removeSlot);
#if defined(_DEBUG)
  const long patch_n = ++g_patchCalls;
  if (patch_n <= 16 || (patch_n % 256) == 0) {
    logDebugF("custom: vtable patches=%ld clones=%zu", patch_n, cloneCount());
  }
#endif
  *reinterpret_cast<void***>(effectObject) = clone;
  return block;
}

// Factory Create slot replacement: call the template Create first (argument parsing identical to
// the engine's), then replace the object's vtable slots per the registered implementation. Fully
// generalized; it knows no concrete behavior.
extern "C" void* ykkz000_customFactoryCreate(void* self, void* outSharedPtr,
                                             const void* params) {
  if (self == nullptr || outSharedPtr == nullptr) {
    return outSharedPtr;
  }
  const auto typeHash =
      TryReadOr(self, &civ6::ModifierEffectFactory::type_hash, std::uint32_t{0});
  EffectRecord record;
  if (!findEffectRecord(typeHash, record) || record.originalCreate == nullptr) {
    logError("Custom effect: missing factory implementation record");
    return outSharedPtr;
  }
  reinterpret_cast<FactoryCreateFn>(record.originalCreate)(self, outSharedPtr, params);
  void* object = *reinterpret_cast<void**>(outSharedPtr);
  if (object != nullptr) {
    (void)patchEffectObjectSlots(object, typeHash);
  }
  return outSharedPtr;
}

void* customFactoryCreateEntry() {
  return reinterpret_cast<void*>(&ykkz000_customFactoryCreate);
}

void teardownPluginEffects(void* pluginHandle) {
  if (pluginHandle == nullptr) {
    return;
  }
  // 1) Restore the effect-object vtable slots this plugin replaced (the template original
  //    functions still point into the engine, so this is safe).
  {
    std::lock_guard<std::mutex> guard(g_cloneMutex);
    for (CloneRecord& record : g_clones) {
      if (record.pluginHandle != pluginHandle) {
        continue;
      }
      for (const auto& entry : record.replaced) {
        record.clone[entry.first] = const_cast<void*>(entry.second);
      }
      record.replaced.clear();
      record.pluginHandle = nullptr;
    }
  }
  // 2) Clear the implementation callback pointers this plugin registered (label/userData are also
  //    plugin memory, cleared as well).
  {
    std::lock_guard<std::mutex> guard(g_recordMutex);
    for (auto& pair : g_records) {
      EffectRecord& record = pair.second;
      if (record.pluginHandle != pluginHandle) {
        continue;
      }
      record.impl.apply = nullptr;
      record.impl.remove = nullptr;
      record.impl.analyze = nullptr;
      record.impl.handlerApply = nullptr;
      record.impl.label = nullptr;
      record.impl.userData = nullptr;
      record.pluginHandle = nullptr;
    }
  }
  clearHandlerCallbacksForPlugin(pluginHandle);
}

} // namespace ykkz000::loader
