#include <windows.h>

#include <intrin.h> // _ReturnAddress

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <ykkz000/loader/internal.h>

// Effect handler registration.
//
// The engine keeps two registries for "effect runtime behavior": the factory table
// (Registry<IModifierEffectFactory>, already extended by registry.cpp) and the handler table
// (built by FUN_1804891b0). When a custom EffectType is registered only in the factory table and
// not in the handler table, the engine resolves its behavior handler to an invalid node and
// dereferences 0xFFFFFFFF. This file re-registers already-registered effect type hashes into the
// handler table after it has been built.
//
// Each custom effect clones its template handler's descriptor table, with slots 0/1 pointing at
// this file's safe wrappers; when the input is valid and the plugin provided analyze/handlerApply,
// the wrapper forwards to the plugin (wrapped in SEH), otherwise it forwards to the template.
namespace ykkz000::loader {
namespace {

using HandlerRegistryInitFn = void* (*)(void* root);
using SetEffectHandlerFn =
    void (*)(void* root, int kind, std::uint32_t hash, void* handlerObject);
using HandlerNodeInsertFn = void* (*)(void* container, void* outNode,
                                      const std::uint32_t* hash);
using TemplateAnalyzeFn = void* (*)(void* self, void* args);
using TemplateApplyFn = std::uint64_t (*)(void* self, void* context, void* args);
#if defined(ENABLE_DISPATCH_TRACE)
using HandlerDispatchFn = void* (*)(void* node);
#endif

HandlerRegistryInitFn g_originalHandlerRegistryInit = nullptr;
SetEffectHandlerFn    g_originalSetEffectHandler = nullptr;
HandlerNodeInsertFn   g_originalHandlerNodeInsert = nullptr;
TemplateAnalyzeFn     g_originalTemplateAnalyze = nullptr;
TemplateApplyFn       g_originalTemplateApply = nullptr;
#if defined(ENABLE_DISPATCH_TRACE)
HandlerDispatchFn     g_originalHandlerDispatch = nullptr;
#endif
void*                 g_handlerRoot = nullptr;
// The root used by the most recent re-registration. The engine's runtime handler table and the
// init-time table (g_handlerRoot) are different objects, so we must re-register against the latest
// root, otherwise lookups will not find our effects.
void*                 g_registeredRoot = nullptr;

// Multi-template support: each custom effect reuses its own template effect (such as the city
// yield modifier / player combat-strength modifier). At runtime the nodes are captured from the
// engine registry per template hash, then each effect clones its own descriptor table and handler
// object, avoiding applying template A's apply to effect B.
//
// Note: template handlers are always captured at runtime; the profile's data-class RVAs are not
// used (they do not match across runs).
std::unordered_map<std::uint32_t, void*> g_templateNodes;         // templateHash -> node
std::unordered_map<std::uint32_t, void*> g_customHandlerObjects;  // effectHash -> self-built object
// Self-built handler object -> the original implementations of the template analyze/apply in its
// clone table (for wrapper forwarding), plus the plugin-provided replacement callbacks and their
// owning handle. The key must be the handler object itself; handler[0] is the descriptor table.
struct HandlerOriginals {
  void* analyze = nullptr;
  void* apply = nullptr;
  bridge::AnalyzeFn pluginAnalyze = nullptr;
  bridge::ApplyFn pluginApply = nullptr;
  void* pluginHandle = nullptr;
};
std::unordered_map<void*, HandlerOriginals> g_handlerOriginals;

// >0 means we are inside our own re-registration call, used to prevent the setEffectHandler hook
// from re-registering recursively.
int                   g_registerDepth = 0;
bool                  g_handlerHookInstalled = false;
std::mutex            g_handlerMutex;
std::mutex            g_hookMutex;

// Diagnostic instrumentation throttling parameters: print everything for the first several calls,
// then sample by stride, to avoid log spam when population changes frequently.
constexpr long kTraceFullCalls = 32;
constexpr long kTraceStride = 256;

// Dispatch-hook throttling parameters: print everything for the first N calls, then sample.
// Invalid handlers are always printed.
#if defined(ENABLE_DISPATCH_TRACE)
constexpr long kDispatchFullCalls = 64;
constexpr long kDispatchStride = 256;
#endif

// Dereference only clearly valid pointers, so the instrumentation itself cannot trigger a second
// crash on an address like 0xFFFFFFFF.
bool isPlausiblePointer(const void* pointer) {
  return reinterpret_cast<std::uintptr_t>(pointer) > 0x10000;
}

// Snapshot of the handler caches for diagnostics: the sizes are copied under g_handlerMutex and the
// caller logs after the lock is released (never nest the log lock inside the business lock).
struct HandlerTableSizes {
  std::size_t templates = 0;
  std::size_t customs = 0;
  std::size_t originals = 0;
};

HandlerTableSizes handlerTableSizes() {
  std::lock_guard<std::mutex> guard(g_handlerMutex);
  return HandlerTableSizes{g_templateNodes.size(), g_customHandlerObjects.size(),
                           g_handlerOriginals.size()};
}

// FUN_1806083f0(root, 2, hash, handler)'s first batch of memory accesses: root+0x08/+0x18/+0x30,
// plus the bucket array *(root+0x18), length (mask+1)*0x10, mask = *(root+0x30).
// If any of these does not hold, the root is treated as stale and never handed to the engine
// (root has been freed when exiting the whole game).
bool isPlausibleRegistryRoot(const void* root) {
  // root's +0x00 is the effect table (kind=2) hash-table header (see civ6::HandlerRegistryRoot).
  constexpr std::size_t kMaskEnd =
      offsetof(civ6::HandlerHashTable, mask) + sizeof(std::uint64_t);
  if (!isReadableRegion(root, kMaskEnd)) {
    return false;
  }
  const std::size_t tableOffset = offsetof(civ6::HandlerRegistryRoot, effects);
  std::uint64_t mask = 0;
  (void)tryReadField(root, tableOffset + offsetof(civ6::HandlerHashTable, mask), mask);
  void* buckets = nullptr;
  (void)tryReadField(root, tableOffset + offsetof(civ6::HandlerHashTable, buckets), buckets);
  constexpr std::uint64_t kMaxBucketCount = 0x10000; // guard: an anomalous mask is treated as invalid
  if (mask >= kMaxBucketCount || buckets == nullptr) {
    return false;
  }
  return isReadableRegion(buckets, static_cast<std::size_t>((mask + 1) * civ6::kHandlerBucketBytes));
}

// Read a value only after validating readability; on failure return fallback. Shared by the
// instrumentation and node parsing, so neither crashes on an unreadable address.
int readInt32(const void* base, std::size_t offset, int fallback = 0) {
  int value = fallback;
  (void)tryReadField(base, offset, value);
  return value;
}

void* readPointer(const void* base, std::size_t offset) {
  void* value = nullptr;
  (void)tryReadField(base, offset, value);
  return value;
}

// -- SEH-wrapped plugin callbacks --
// MSVC forbids __try in a function that contains C++ objects requiring stack unwinding, so this
// stays a thin POD-only wrapper; when the VEH sees g_guardedCallActive it passes through and lets
// __except take over (no crash log written).
void* callGuardedAnalyze(bridge::AnalyzeFn fn, void* self, void* args) {
  g_guardedCallActive = true;
  void* result = nullptr;
  __try {
    result = fn(self, args);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    result = nullptr;
  }
  g_guardedCallActive = false;
  return result;
}

std::uint64_t callGuardedApply(bridge::ApplyFn fn, void* self, void* a1, void* a2, void* a3) {
  g_guardedCallActive = true;
  std::uint64_t result = 0;
  __try {
    result = fn(self, a1, a2, a3);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    result = 0;
  }
  g_guardedCallActive = false;
  return result;
}

// -- Query/apply path instrumentation: print only (throttled), do not change behavior --
std::atomic<long> s_setCalls{0};
std::atomic<long> s_analyzeCalls{0};
std::atomic<long> s_applyCalls{0};

// Re-register the effects registered in this process into the handler table (kind=2) of the given
// root. Idempotent and safe to call repeatedly. Defined later in this file; SetEffectHandler_Hook
// needs a forward declaration.
int registerCustomEffectHandlersForRoot(void* root);

void SetEffectHandler_Hook(void* root, int kind, std::uint32_t hash, void* handlerObject) {
  const long call = ++s_setCalls;
  if (call <= kTraceFullCalls || (call % kTraceStride) == 0) {
    const HandlerTableSizes sizes = handlerTableSizes();
    logDebugF("set: call#%ld root=%p kind=%d hash=0x%08X handler=%p templates=%zu customs=%zu "
              "originals=%zu",
              call, root, kind, hash, handlerObject, sizes.templates, sizes.customs,
              sizes.originals);
  }
  // Let the engine's own registration take effect first, then mirror the re-registration, to avoid
  // modifying the same container during its internal iteration.
  if (g_originalSetEffectHandler != nullptr) {
    g_originalSetEffectHandler(root, kind, hash, handlerObject);
  }

  // The engine's runtime handler table and the init-time table are different roots. Whenever a
  // registration happens on a root not yet re-registered (i.e. the runtime table's first
  // appearance), immediately re-register our effects against the same root; it is idempotent and
  // recursion-safe.
  if (root != nullptr && g_registerDepth == 0) {
    bool needRegister = false;
    {
      std::lock_guard<std::mutex> guard(g_handlerMutex);
      if (g_registeredRoot != root) {
        g_registeredRoot = root;
        needRegister = true;
      }
    }
    if (needRegister) {
      ++g_registerDepth;
      logInfoF("handler: runtime root detected root=%p (from kind=%d hash=0x%08X)",
               root, kind, hash);
      registerCustomEffectHandlersForRoot(root);
      --g_registerDepth;
    }
  }
}

// hook FUN_180489040(container, outNode, hashPtr): handler table insert/lookup. Whether inserting
// or looking up, the function writes the node into outNode[0]; so on a hit for any
// "already-registered template hash", capture its node, and afterwards read node+0x18 to obtain
// the handler object the engine actually uses (the function is called with that value at
// registration time).
void* HandlerNodeInsert_Hook(void* container, void* outNode, const std::uint32_t* hash) {
  void* result = g_originalHandlerNodeInsert != nullptr
                     ? g_originalHandlerNodeInsert(container, outNode, hash)
                     : outNode;
  if (hash != nullptr && outNode != nullptr && isRegisteredTemplateHash(*hash)) {
    void* node = readPointer(outNode, 0); // outNode[0] = node pointer
    if (node != nullptr) {
      {
        std::lock_guard<std::mutex> guard(g_handlerMutex);
        g_templateNodes[*hash] = node;
      }
      logDebugF("handler: template effect node captured template=0x%08X node=%p", *hash,
                node);
    }
  }
  return result;
}

// Get the "engine-actually-used" template handler object (called at registration time, when the
// engine has written node+0x18).
void* templateEffectHandlerObject(std::uint32_t templateHash) {
  void* node = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    const auto it = g_templateNodes.find(templateHash);
    if (it != g_templateNodes.end()) {
      node = it->second;
    }
  }
  if (isPlausiblePointer(node)) {
    void* handler = readPointer(node, offsetof(civ6::HandlerNode, handler)); // node + 0x18
    if (isPlausiblePointer(handler)) {
      return handler;
    }
  }
  logErrorF("handler: template node 0x%08X not captured; handler unavailable", templateHash);
  return nullptr;
}

// During shutdown, context/args may be 0 or already invalid: validate first; if invalid, no-op
// immediately and never enter the template implementation (the template apply dereferences fields
// like [x+0x81], so a 0 input crashes it). This is the decisive fix for the exit crash: we cannot
// rely on destroyGameContext removing the node -- the shutdown path may bypass it and call the
// handler directly.
// Look up the template's original slot via self (the handler object) and forward; self is the
// handler object, handler[0] is the descriptor table, and the descriptor table must never be used
// as the key. When a plugin callback exists, forward to the plugin first (wrapped in SEH).
extern "C" void* ykkz000_handlerAnalyze(void* self, void* args) {
  if (!isPlausiblePointer(args)) {
    return nullptr;
  }
  HandlerOriginals originals;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    const auto it = g_handlerOriginals.find(self);
    if (it != g_handlerOriginals.end()) {
      originals = it->second;
    }
  }
  if (originals.pluginAnalyze != nullptr) {
    return callGuardedAnalyze(originals.pluginAnalyze, self, args);
  }
  return originals.analyze != nullptr
             ? reinterpret_cast<TemplateAnalyzeFn>(originals.analyze)(self, args)
             : nullptr;
}

extern "C" std::uint64_t ykkz000_handlerApply(void* self, void* context, void* args) {
  if (!isPlausiblePointer(args) || !isPlausiblePointer(context)) {
    return 0; // shutdown-safe branch: do not touch the template/plugin implementation anymore
  }
  HandlerOriginals originals;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    const auto it = g_handlerOriginals.find(self);
    if (it != g_handlerOriginals.end()) {
      originals = it->second;
    }
  }
  if (originals.pluginApply != nullptr) {
    return callGuardedApply(originals.pluginApply, self, context, args, nullptr);
  }
  return originals.apply != nullptr
             ? reinterpret_cast<TemplateApplyFn>(originals.apply)(self, context, args)
             : 0;
}

// The self-built handler object holds no engine resources, so releasing it is a no-op. It must
// never forward to the template release routine (FUN_18046b610): that dereferences handler-object
// fields, and our object is not a template object.
extern "C" std::uint64_t ykkz000_handlerRelease(void* /*self*/, int /*flags*/) {
  return 0;
}

// Build/reuse the handler object for the given effect: clone the template descriptor table, replace
// slots 0/1 with the safe wrappers above and slot 4 with the no-op release, and copy the remaining
// slots verbatim (keeping other behavior unchanged). The object is zeroed and allocated large
// enough, so the engine reading object fields during shutdown does not get heap garbage. On failure
// return nullptr (the caller skips registration; never register an empty handler that would make
// the engine dereference an invalid node).
void* customEffectHandlerObject(std::uint32_t effectHash, std::uint32_t templateHash,
                               const bridge::EffectImpl& impl, void* pluginHandle) {
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    const auto it = g_customHandlerObjects.find(effectHash);
    if (it != g_customHandlerObjects.end()) {
      return it->second;
    }
  }
  void* handler = templateEffectHandlerObject(templateHash);
  if (!isPlausiblePointer(handler)) {
    logErrorF("handler: template 0x%08X handler unavailable; clone aborted", templateHash);
    return nullptr;
  }
  void* table = readPointer(handler, offsetof(civ6::HandlerObject, table)); // handler[0] = descriptor table
  if (!isPlausiblePointer(table)) {
    logErrorF("handler: template 0x%08X handler table invalid; clone aborted", templateHash);
    return nullptr;
  }

  auto* clone = static_cast<void**>(
      HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * kHandlerTableCloneSlots));
  if (clone == nullptr) {
    return nullptr;
  }
  std::memcpy(clone, table, sizeof(void*) * kHandlerTableCloneSlots);
  HandlerOriginals originals;
  originals.analyze = clone[civ6::kHandlerAnalyzeSlot];
  originals.apply = clone[civ6::kHandlerApplySlot];
  originals.pluginAnalyze = impl.analyze;
  originals.pluginApply = impl.handlerApply;
  originals.pluginHandle = pluginHandle;
  clone[civ6::kHandlerAnalyzeSlot] = reinterpret_cast<void*>(&ykkz000_handlerAnalyze);
  clone[civ6::kHandlerApplySlot] = reinterpret_cast<void*>(&ykkz000_handlerApply);
  clone[civ6::kHandlerReleaseSlot] = reinterpret_cast<void*>(&ykkz000_handlerRelease);

  // Zero + large enough: the handler object is not a bare 8 bytes, so other slots do not read heap
  // garbage.
  auto* handlerObject = static_cast<std::uint8_t*>(
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(civ6::HandlerObject)));
  if (handlerObject == nullptr) {
    HeapFree(GetProcessHeap(), 0, clone);
    return nullptr;
  }
  (void)TryWrite(handlerObject, &civ6::HandlerObject::table,
                 reinterpret_cast<civ6::HandlerTable*>(clone));

  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    g_handlerOriginals[handlerObject] = originals;
    g_customHandlerObjects[effectHash] = handlerObject;
  }

  logDebugF(
              "handler: custom handler cloned effect=0x%08X template=0x%08X table=%p clone=%p "
              "analyze=%p apply=%p release=%p plugin=%p/%p",
              effectHash, templateHash, table, clone, originals.analyze, originals.apply,
              reinterpret_cast<void*>(&ykkz000_handlerRelease),
              reinterpret_cast<void*>(originals.pluginAnalyze),
              reinterpret_cast<void*>(originals.pluginApply));
  return handlerObject;
}

void* TemplateAnalyze_Hook(void* self, void* args) {
  const long call = ++s_analyzeCalls;
  if (call <= kTraceFullCalls || (call % kTraceStride) == 0) {
    const HandlerTableSizes sizes = handlerTableSizes();
    logDebugF("analyze: call#%ld self=%p args=%p templates=%zu customs=%zu originals=%zu", call,
              self, args, sizes.templates, sizes.customs, sizes.originals);
  }
  return g_originalTemplateAnalyze != nullptr ? g_originalTemplateAnalyze(self, args) : nullptr;
}

std::uint64_t TemplateApply_Hook(void* self, void* context, void* args) {
  const long call = ++s_applyCalls;
  if (call <= kTraceFullCalls || (call % kTraceStride) == 0) {
    int amount = -1;
    int yieldType = -1;
    void* city = nullptr;
    int population = -1;
    if (isPlausiblePointer(args)) {
      amount = readInt32(args, offsetof(civ6::EffectArgs, amount));
      yieldType = readInt32(args, offsetof(civ6::EffectArgs, yield_type));
    }
    if (isPlausiblePointer(context)) {
      city = readPointer(context, offsetof(civ6::EffectContext, city));
      if (isPlausiblePointer(city)) {
        population = readInt32(city, offsetof(civ6::City::Instance, population));
      }
    }
    const HandlerTableSizes sizes = handlerTableSizes();
    logDebugF(
                "apply: call#%ld self=%p context=%p args=%p amount=%d yield=%d city=%p pop=%d "
                "templates=%zu customs=%zu originals=%zu",
                call, self, context, args, amount, yieldType, city, population, sizes.templates,
                sizes.customs, sizes.originals);
  }
  return g_originalTemplateApply != nullptr ? g_originalTemplateApply(self, context, args) : 0;
}

// handler dispatch thunk (RVA 0x979290):
//   handler = *(node+0x18); if (handler) { table = *(handler); return table[6](handler); } return 1;
// Note: this is not effect-handler dispatch, but a generic node->object->virtual-call-slot +0x30
// helper; it is diagnostic only and is not installed by default (rebuild with
// -DENABLE_DISPATCH_TRACE=ON when evidence is needed).
#if defined(ENABLE_DISPATCH_TRACE)
void* HandlerDispatch_Hook(void* node) {
  static std::atomic<long> s_dispatchCalls{0};
  const long call = ++s_dispatchCalls;

  const bool nodeOk = isPlausiblePointer(node);
  const void* handler = nodeOk ? readPointer(node, offsetof(civ6::HandlerNode, handler)) : nullptr;
  const void* table =
      isPlausiblePointer(handler) ? readPointer(handler, offsetof(civ6::HandlerObject, table))
                                  : nullptr;
  const void* slotSix =
      isPlausiblePointer(table) ? readPointer(table, offsetof(civ6::HandlerTable, dispatch_slot))
                                : nullptr;
  const std::uint32_t hash =
      nodeOk ? static_cast<std::uint32_t>(readInt32(node, offsetof(civ6::HandlerNode, hash))) : 0;

  // handler non-null but unusable => exactly the situation that produces the 0xFFFFFFFF read.
  const bool invalid =
      (handler != nullptr) &&
      (!isPlausiblePointer(handler) || !isPlausiblePointer(table) || !isPlausiblePointer(slotSix));

  if (call <= kDispatchFullCalls || (call % kDispatchStride) == 0 || invalid) {
    std::uintptr_t callerRva = 0;
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(gameCore().module);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (moduleBase != 0 && caller > moduleBase) {
      callerRva = caller - moduleBase;
    }
    if (invalid) {
      logErrorF("dispatch: call#%ld node=%p hash=0x%08X handler=%p table=%p slot6=%p "
                "caller_rva=0x%zX  <-- INVALID (guard: return 1)",
                call, node, hash, handler, table, slotSix, callerRva);
    } else {
      logDebugF("dispatch: call#%ld node=%p hash=0x%08X handler=%p table=%p slot6=%p "
                "caller_rva=0x%zX",
                call, node, hash, handler, table, slotSix, callerRva);
    }
  }

#if !defined(DISABLE_HANDLER_GUARD)
  if (invalid) {
    return reinterpret_cast<void*>(1); // the engine's own "no handler" semantics
  }
#endif
  return g_originalHandlerDispatch != nullptr ? g_originalHandlerDispatch(node)
                                              : reinterpret_cast<void*>(1);
}
#endif

// Re-register the effects registered in this process into the handler table (kind=2) of the given
// root. Returns the number of entries re-registered.
int registerCustomEffectHandlersForRoot(void* root) {
  const GameCoreApi& api = gameCore();
  if (root == nullptr || api.setEffectHandler == nullptr) {
    logError("handler: registry unavailable; custom effect handlers not registered");
    return 0;
  }

  const std::vector<RegisteredEffect> effects = registeredEffects();
  int count = 0;
  for (const RegisteredEffect& effect : effects) {
    if (effect.hash == 0) {
      continue;
    }
    EffectRecord record;
    (void)findEffectRecord(effect.hash, record); // no implementation record means pure template reuse
    // Register the effect's self-built handler (cloned template descriptor table + safe wrappers
    // for analyze/apply): when the engine calls it directly during shutdown, invalid inputs are
    // short-circuited by the wrapper and no longer enter the template implementation and crash.
    void* handlerObject =
        customEffectHandlerObject(effect.hash, effect.templateHash, record.impl,
                                  record.pluginHandle);
    if (handlerObject == nullptr) {
      logErrorF("handler: no handler for hash=0x%08X template=0x%08X; skipped",
                effect.hash, effect.templateHash);
      continue;
    }
    reinterpret_cast<SetEffectHandlerFn>(api.setEffectHandler)(
        root, kHandlerKindEffects, effect.hash, handlerObject);
    logInfoF(
                "handler: registered hash=0x%08X kind=%d root=%p handler=%p template=0x%08X "
                "type=%s",
                effect.hash, kHandlerKindEffects, root, handlerObject, effect.templateHash,
                effect.typeName.c_str());
    ++count;
  }
  logInfoF("handler: registration complete root=%p count=%d", root, count);
  {
    const HandlerTableSizes sizes = handlerTableSizes();
    logInfoF("handler: register root=%p count=%d templates=%zu", root, count, sizes.templates);
  }
  return count;
}

// Remove the handler nodes we registered on the runtime table root (handlerObject=nullptr =>
// the engine removes that node). Call only before the real DllDestroyGameContext. When exiting the
// whole game the engine may have already torn down the registry, in which case the saved root
// memory is unreadable: it must be validated first with isPlausibleRegistryRoot and never handed
// to the engine (otherwise FUN_1806083f0 reads root+0x30 and crashes; see exit-crash record #2).
void removeCustomEffectHandlersImpl() {
  const GameCoreApi& api = gameCore();
  if (api.setEffectHandler == nullptr) {
    return;
  }
  void* root = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    root = g_registeredRoot; // the init root is only valid during table construction; caught below if unreadable
  }
  if (root == nullptr) {
    logInfo("handler: no registry root recorded; skip removal");
    return;
  }
  if (!isPlausibleRegistryRoot(root)) {
    // When exiting the whole game the engine may have already torn down the registry, leaving the
    // memory unreadable; skip in that case. Slot 4 is already a no-op, so skipping the removal
    // does not cause a shutdown crash.
    logWarnF("handler: skip stale root %p; removal aborted", root);
    return;
  }
  const std::vector<RegisteredEffect> effects = registeredEffects();
  int removed = 0;
  for (const RegisteredEffect& effect : effects) {
    if (effect.hash == 0) {
      continue;
    }
    reinterpret_cast<SetEffectHandlerFn>(api.setEffectHandler)(
        root, kHandlerKindEffects, effect.hash, nullptr);
    ++removed;
  }
  logInfoF("handler: custom handlers removed root=%p count=%d", root, removed);
}

// hook FUN_1804891b0: RCX is the handler table root. The original function must first build all
// built-in nodes, then we re-register our effects into it; each game context rebuilds the root
// object, so registration must be repeated every time.
//
// Note: decompilation confirms the original function returns its root argument in RAX, so keep a
// void* return here and return the trampoline result.
void* HandlerRegistryInit_Hook(void* root) {
  if (g_originalHandlerRegistryInit == nullptr) {
    return root;
  }
  logDebugF("registry-init: root=%p", root);
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    g_handlerRoot = root;
    g_registeredRoot = root; // the init table has been re-registered
  }
  void* result = g_originalHandlerRegistryInit(root);
  registerCustomEffectHandlersForRoot(root);
  return result;
}

} // namespace

void removeCustomEffectHandlers() {
  removeCustomEffectHandlersImpl();
}

void clearHandlerCallbacksForPlugin(void* pluginHandle) {
  if (pluginHandle == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_handlerMutex);
  for (auto& pair : g_handlerOriginals) {
    HandlerOriginals& originals = pair.second;
    if (originals.pluginHandle != pluginHandle) {
      continue;
    }
    originals.pluginAnalyze = nullptr;
    originals.pluginApply = nullptr;
    originals.pluginHandle = nullptr;
  }
}

bool installEffectHandlerHook() {
#if defined(DISABLE_EFFECT_HANDLER)
  logInfo("Effect handler hook: disabled by DISABLE_EFFECT_HANDLER");
  return false;
#else
  std::lock_guard<std::mutex> guard(g_hookMutex);
  if (g_handlerHookInstalled) {
    return true;
  }
  LogScope scope("install effect handler hook");
  const GameCoreApi& api = gameCore();
  logDebugF("handler: registryInit=%p setHandler=%p handlerData=%p table=%p analyze=%p apply=%p",
            api.handlerRegistryInit, api.setEffectHandler, api.profiledHandlerData,
            api.profiledHandlerTable, api.profiledHandlerAnalyze, api.profiledHandlerApply);
  if (api.handlerRegistryInit == nullptr || api.setEffectHandler == nullptr) {
    logError("Effect handler hook: entry points unavailable; custom effects may crash "
             "when the engine resolves their handler");
    return false;
  }
  if (!ensureHookServiceInitialized()) {
    logFatal("Effect handler hook: MinHook initialization failed");
    return false;
  }

  auto addHook = [](void* target, LPVOID detour, LPVOID* trampoline, const char* name) {
    if (target == nullptr) {
      logWarnF("handler: hook %s target unavailable; skipped", name);
      return;
    }
    const int status = installHookRaw(target, detour, reinterpret_cast<void**>(trampoline));
    if (status != 0) {
      logErrorF("handler: install hook %s -> %d", name, status);
      return;
    }
    logInfoF("handler: hook %s installed target=%p trampoline=%p", name, target, *trampoline);
  };

  addHook(api.handlerRegistryInit, reinterpret_cast<LPVOID>(&HandlerRegistryInit_Hook),
          reinterpret_cast<LPVOID*>(&g_originalHandlerRegistryInit), "handlerRegistryInit");

  // Diagnostic instrumentation (print only, do not change behavior): confirm whether the engine
  // routes custom effects through the handler apply path. Failure is not treated as fatal -- the
  // re-registration itself does not depend on these three hooks.
  addHook(api.setEffectHandler, reinterpret_cast<LPVOID>(&SetEffectHandler_Hook),
          reinterpret_cast<LPVOID*>(&g_originalSetEffectHandler), "setEffectHandler");
  addHook(api.handlerNodeInsert, reinterpret_cast<LPVOID>(&HandlerNodeInsert_Hook),
          reinterpret_cast<LPVOID*>(&g_originalHandlerNodeInsert), "handlerNodeInsert");
  addHook(api.profiledHandlerAnalyze, reinterpret_cast<LPVOID>(&TemplateAnalyze_Hook),
          reinterpret_cast<LPVOID*>(&g_originalTemplateAnalyze), "profiledHandlerAnalyze");
  addHook(api.profiledHandlerApply, reinterpret_cast<LPVOID>(&TemplateApply_Hook),
          reinterpret_cast<LPVOID*>(&g_originalTemplateApply), "profiledHandlerApply");
#if defined(ENABLE_DISPATCH_TRACE)
  addHook(api.effectHandlerDispatch, reinterpret_cast<LPVOID>(&HandlerDispatch_Hook),
          reinterpret_cast<LPVOID*>(&g_originalHandlerDispatch), "handlerDispatch");
#endif

  g_handlerHookInstalled = true;
  logInfo("Effect handler hook: installed");
  return true;
#endif
}

// Uninstall only disables the hooks; it does not free MinHook trampolines (same policy as the other
// hooks: during uninstall a thread may still be executing inside the detour, and freeing the
// trampoline would call into freed memory). The next game context re-enables them via
// installEffectHandlerHook().
void uninstallEffectHandlerHook() {
  std::lock_guard<std::mutex> guard(g_hookMutex);
  LogScope scope("uninstall effect handler hook");
  if (g_handlerHookInstalled) {
    const GameCoreApi& api = gameCore();
    if (api.handlerRegistryInit != nullptr) {
      (void)removeHookRaw(api.handlerRegistryInit);
    }
    if (api.setEffectHandler != nullptr) {
      (void)removeHookRaw(api.setEffectHandler);
    }
    if (api.handlerNodeInsert != nullptr) {
      (void)removeHookRaw(api.handlerNodeInsert);
    }
    if (api.profiledHandlerAnalyze != nullptr) {
      (void)removeHookRaw(api.profiledHandlerAnalyze);
    }
    if (api.profiledHandlerApply != nullptr) {
      (void)removeHookRaw(api.profiledHandlerApply);
    }
#if defined(ENABLE_DISPATCH_TRACE)
    if (api.effectHandlerDispatch != nullptr) {
      (void)removeHookRaw(api.effectHandlerDispatch);
    }
#endif
    g_handlerHookInstalled = false;
  }
  {
    std::lock_guard<std::mutex> rootGuard(g_handlerMutex);
    g_handlerRoot = nullptr;
    g_registeredRoot = nullptr;
    g_registerDepth = 0;
    // Old clones, old handler objects, and the original-slot map are not freed (the engine
    // registry may still reference them, so they must live for the process lifetime); only the
    // caches are cleared so the next context rebuilds from newly captured template nodes. After
    // the original-slot map is cleared, if a thread still calls an old handler's safe wrapper
    // during shutdown, it safely no-ops instead of forwarding to a freed template implementation.
    g_templateNodes.clear();
    g_customHandlerObjects.clear();
    g_handlerOriginals.clear();
  }
}

} // namespace ykkz000::loader
