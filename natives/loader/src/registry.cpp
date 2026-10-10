#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <ykkz000/loader/internal.h>

namespace ykkz000::loader {
namespace {

std::mutex g_registryMutex;
std::unordered_set<std::uint32_t> g_registeredHashes;
std::vector<RegisteredEffect> g_registeredEffects;

using MallocTempFn = void* (*)(std::size_t size, const char* file, int line, int a, int b);
using ReserveVectorFn = void (*)(void* vector, std::size_t count);

void* readPointer(void* base, std::size_t offset) {
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + offset);
}

void writePointer(void* base, std::size_t offset, void* value) {
  *reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + offset) = value;
}

// Read the factory object's type hash directly at +0x08; never call an engine virtual function:
// `GetTypeId()` is equivalent to returning that field, and calling a vtable with a guessed
// signature once triggered a /GS stack-cookie fast fail.
std::uint32_t typeIdOf(void* factory) {
  if (factory == nullptr) {
    return 0;
  }
  return TryReadOr(factory, &civ6::ModifierEffectFactory::type_hash, std::uint32_t{0});
}

void* findTemplateFactory(void* vector, std::uint32_t templateHash) {
  auto* begin = static_cast<void**>(readPointer(vector, offsetof(civ6::VectorView<void*>, begin)));
  auto* end = static_cast<void**>(readPointer(vector, offsetof(civ6::VectorView<void*>, end)));
  const std::ptrdiff_t count =
      (begin != nullptr && end != nullptr) ? (end - begin) : -1;
  logDebugF("reg: registry begin=%p end=%p count=%lld", begin, end,
            static_cast<long long>(count));
  if (begin == nullptr || end == nullptr || end < begin || count > 4096) {
    logError("reg: registry iterators look invalid; aborting iteration");
    return nullptr;
  }
  for (auto* cursor = begin; cursor != end; ++cursor) {
    if (typeIdOf(*cursor) == templateHash) {
      logDebugF("reg: template matched factory=%p hash=0x%08X", *cursor, templateHash);
      return *cursor;
    }
  }
  logDebugF("reg: template hash 0x%08X not found in %lld entries", templateHash,
            static_cast<long long>(count));
  return nullptr;
}

// Validate that the plugin-provided implementation is self-consistent: declaring a replacement for
// the Apply/Remove slot requires the corresponding template function pointer, otherwise the Loader
// cannot locate the slot in the cloned vtable.
bool isImplConsistent(const bridge::EffectImpl* impl) {
  if (impl == nullptr) {
    return true;
  }
  if (impl->apply != nullptr && impl->templateApply == nullptr) {
    return false;
  }
  if (impl->remove != nullptr && impl->templateRemove == nullptr) {
    return false;
  }
  return true;
}

// Remove the "just appended" factory object from the engine registry vector: step the end pointer
// back one and clear that slot. It only acts when the last entry really points at this factory,
// avoiding deleting someone else's entry in an abnormal state.
void removeLastFactory(void* vector, void* factory) {
  if (vector == nullptr || factory == nullptr) {
    return;
  }
  auto* begin = static_cast<std::uint8_t*>(
      readPointer(vector, offsetof(civ6::VectorView<void*>, begin)));
  auto* end = static_cast<std::uint8_t*>(
      readPointer(vector, offsetof(civ6::VectorView<void*>, end)));
  if (begin == nullptr || end == nullptr || end <= begin) {
    return;
  }
  auto* last = reinterpret_cast<void**>(end) - 1;
  if (*last != factory) {
    return;
  }
  *last = nullptr;
  writePointer(vector, offsetof(civ6::VectorView<void*>, end), last);
}

// Failure rollback for registerEffectType; the caller must hold g_registryMutex: undo every trace
// of this registration (factory vector entry, typeHash, registered-effect record, impl record, and
// type name). Parts that were not successfully registered are no-ops.
void rollbackRegistrationLocked(std::uint32_t typeHash, void* vector, void* factory) {
  logWarnF("registerEffectType: rolling back hash=0x%08X", typeHash);
  removeLastFactory(vector, factory);
  (void)unregisterEffectImpl(typeHash);
  forgetTypeName(typeHash);
  g_registeredHashes.erase(typeHash);
  for (auto it = g_registeredEffects.begin(); it != g_registeredEffects.end(); ++it) {
    if (it->hash == typeHash) {
      g_registeredEffects.erase(it);
      break;
    }
  }
}

} // namespace

std::vector<RegisteredEffect> registeredEffects() {
  std::lock_guard<std::mutex> guard(g_registryMutex);
  return g_registeredEffects;
}

bool isRegisteredTemplateHash(std::uint32_t hash) {
  if (hash == 0) {
    return false;
  }
  std::lock_guard<std::mutex> guard(g_registryMutex);
  for (const RegisteredEffect& effect : g_registeredEffects) {
    if (effect.templateHash == hash) {
      return true;
    }
  }
  return false;
}

// The custom effect's own type hash (factory object +0x08), distinct from the template hash of
// isRegisteredTemplateHash. Template-Create bypass diagnostics use this to label objects as
// custom / built-in.
bool isRegisteredHash(std::uint32_t typeHash) {
  if (typeHash == 0) {
    return false;
  }
  std::lock_guard<std::mutex> guard(g_registryMutex);
  return g_registeredHashes.find(typeHash) != g_registeredHashes.end();
}

int registerEffectType(const bridge::EffectDesc* desc) {
  LogScope scope("register effect type");
  if (desc == nullptr || desc->typeName == nullptr || desc->templateEffect == nullptr) {
    logError("registerEffectType: invalid argument");
    return -1;
  }
  if (!isImplConsistent(desc->impl)) {
    logError("registerEffectType: impl declares a slot replacement without template fn");
    return -8;
  }

  const bool custom = desc->impl != nullptr;
  logInfoF("reg: enter type=%s template=%s custom=%d", desc->typeName,
           desc->templateEffect, custom ? 1 : 0);

  if (!ensureGameCoreLoaded()) {
    logError("registerEffectType: real GameCore unavailable");
    return -2;
  }
  logInfoF("reg: GameCore ready");
  const GameCoreApi& api = gameCore();
  if (api.getEffectRegistry == nullptr || api.mallocTemp == nullptr) {
    logError("registerEffectType: GameCore entry points missing");
    return -2;
  }

  const std::uint32_t typeHash = makeHash(desc->typeName);
  const std::uint32_t templateHash = makeHash(desc->templateEffect);
  logDebugF("reg: typeHash=0x%08X templateHash=0x%08X", typeHash, templateHash);

  std::lock_guard<std::mutex> guard(g_registryMutex);
  logInfo("reg: registry lock acquired");
  if (g_registeredHashes.find(typeHash) != g_registeredHashes.end()) {
    return 0; // already registered; idempotent
  }

  const auto getRegistry = reinterpret_cast<bridge::GetEffectRegistryFn>(api.getEffectRegistry);
  logDebugF("reg: calling GetTypes @%p", api.getEffectRegistry); // last line before a crash
  void* vector = getRegistry();
  logDebugF("reg: GetTypes returned vector=%p", vector);         // next line after the crash
  void* templateFactory = findTemplateFactory(vector, templateHash);
  logDebugF("reg: templateFactory=%p", templateFactory);
  if (templateFactory == nullptr) {
    logError("registerEffectType: template effect factory not found");
    return -3;
  }

  void* vtable = cloneFactoryVTable(templateFactory);
  logDebugF("reg: factory vtable clone=%p", vtable);
  if (vtable == nullptr) {
    logError("registerEffectType: failed to clone factory vtable");
    return -4;
  }

  // Custom behavior: point the cloned factory's Create slot at the Loader's generalized entry and
  // register the plugin implementation (the implementation details come from the plugin; the Loader
  // has no behavior branches anymore).
  if (custom) {
    auto* templateVtable = *reinterpret_cast<void***>(templateFactory);
    void* templateCreate =
        templateVtable != nullptr ? templateVtable[civ6::kFactoryCreateSlot] : nullptr;
    logDebugF("reg: custom impl templateCreate=%p", templateCreate);
    if (templateCreate == nullptr) {
      logError("registerEffectType: template factory is missing the Create slot");
      return -8;
    }
    static_cast<void**>(vtable)[civ6::kFactoryCreateSlot] = customFactoryCreateEntry();
    if (registerEffectImpl(typeHash, desc->impl, templateCreate) != 0) {
      logError("registerEffectType: failed to record implementation");
      return -8;
    }
  }

  rememberTypeName(typeHash, desc->typeName);

  auto* factory = reinterpret_cast<MallocTempFn>(api.mallocTemp)(
      sizeof(civ6::ModifierEffectFactory), "ykkz000_loader", 0, 0, 0);
  logDebugF("reg: factory allocated=%p size=0x%zX", factory,
            sizeof(civ6::ModifierEffectFactory));
  if (factory == nullptr) {
    logError("registerEffectType: failed to allocate factory object");
    return -5;
  }
  *reinterpret_cast<void**>(factory) = vtable;
  (void)TryWrite(factory, &civ6::ModifierEffectFactory::type_hash, typeHash);
  logDebugF("reg: factory fields written");

  logDebugF("reg: vector end=%p cap=%p",
            readPointer(vector, offsetof(civ6::VectorView<void*>, end)),
            readPointer(vector, offsetof(civ6::VectorView<void*>, capacity)));
  if (readPointer(vector, offsetof(civ6::VectorView<void*>, end)) ==
      readPointer(vector, offsetof(civ6::VectorView<void*>, capacity))) {
    if (api.reserveVector == nullptr) {
      logError("registerEffectType: registry full and no reserve entry");
      return -6;
    }
    reinterpret_cast<ReserveVectorFn>(api.reserveVector)(vector, 1);
    logDebugF("reg: reserve called");
  }
  logDebugF("reg: vector end after=%p",
            readPointer(vector, offsetof(civ6::VectorView<void*>, end)));

  auto* end = static_cast<std::uint8_t*>(
      readPointer(vector, offsetof(civ6::VectorView<void*>, end)));
  if (end == nullptr) {
    logError("registerEffectType: registry cursor is null");
    return -7;
  }
  *reinterpret_cast<void**>(end) = factory;
  writePointer(vector, offsetof(civ6::VectorView<void*>, end), end + sizeof(void*));

  g_registeredHashes.insert(typeHash);
  g_registeredEffects.push_back(
      RegisteredEffect{typeHash, templateHash, std::string(desc->typeName)});
  logInfoF("reg: done hash=0x%08X", typeHash);

  // The single hook-install entry: at registration time, call the plugin-provided prepare after the
  // factory/implementation have been registered. A non-zero return is treated as a registration
  // failure: first remove the hooks newly created in this batch (fallback; the primary
  // responsibility is prepare revoking its own half-completed installs), then roll back this
  // registration and return the error code.
  if (desc->prepare != nullptr) {
    logInfoF("reg: invoking prepare hash=0x%08X", typeHash);
    beginHookScope();
    const int prepare_status = desc->prepare(desc->userData);
    if (prepare_status != 0) {
      endHookScope(true);
      logErrorF("registerEffectType: prepare returned %d", prepare_status);
      rollbackRegistrationLocked(typeHash, vector, factory);
      return -9;
    }
    endHookScope(false);
  }
  return 0;
}

} // namespace ykkz000::loader
