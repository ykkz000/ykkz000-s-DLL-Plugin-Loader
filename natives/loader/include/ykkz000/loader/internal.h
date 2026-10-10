#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <ykkz000/bridge/host.h>
#include <ykkz000/bridge/log.h>
#include <ykkz000/civ6/common.h>
#include <ykkz000/civ6/effect.h>
#include <ykkz000/civ6/factory.h>
#include <ykkz000/civ6/handler.h>

/// @file internal.h
/// @brief Loader-internal shared declarations: engine entry-point resolution, hook service,
///        memory probing, effect mechanism, and plugin management.
/// @note For use only by the Loader's own .cpp files (not a plugin ABI).
namespace ykkz000::loader {

// -- Engine-side layout --
// The single source of truth for layout is <ykkz000/civ6/*.h>: there, POD data members plus
// static_assert lock down every known offset and size, and vtable slot constants replace raw
// numbers. Call sites must reference those types (offsetof / sizeof / slot constants) and must
// not write field-offset literals again.
// This namespace keeps only "mechanism constants" (clone widths, registry kind, etc.), none of
// which are behavior policy.

/// @brief Effect-object vtable clone width (mechanism constant, not layout).
/// @note Must cover the Apply/Remove and destructor slots; 64 slots is far wider than the actual
///       interface width, and is used to locate and replace slots by function pointer.
constexpr std::size_t kEffectVTableCloneSlots = 64;
/// @brief Handler descriptor-table clone width (16 slots leaves headroom).
constexpr std::size_t kHandlerTableCloneSlots = 16;

/// @brief Handler registry kind: effect table (corresponds to civ6::HandlerRegistryRoot::effects).
constexpr int kHandlerKindEffects = 2;

extern HMODULE g_selfModule;

/// @brief Engine-internal function pointer type (used only inside this DLL).
/// @return GameManager* (FUN_180044d60()).
using GetGameManagerFn = void* (*)();

/// @brief Internal entry points in the real GameCore resolved by signature scanning.
struct GameCoreApi {
  HMODULE module = nullptr;
  void*   getEffectRegistry = nullptr; ///< Registry<IModifierEffectFactory>::GetTypes()
  void*   mallocTemp = nullptr;        ///< Platform::MallocTemp(size, file, line, a, b)
  void*   reserveVector = nullptr;     ///< std::vector::_Reserve(count) member function
  void*   effectApply = nullptr;       ///< Effects::AdjustCityYieldModifier::Apply
  void*   effectRemove = nullptr;      ///< Effects::AdjustCityYieldModifier::Remove
  /// @note Player-unit combat-strength modifier template (optional: when missing, the
  ///       "per suzerain" behavior degrades to no scaling).
  ///       Effects::AdjustPlayerStrengthModifier::Apply / Remove.
  void*   effectStrengthApply = nullptr;
  void*   effectStrengthRemove = nullptr;
  void*   getPlayerByIndex = nullptr;  ///< Reserved: FUN_180044f00(int) (no bounds check; unused)
  void*   getGameManager = nullptr;    ///< FUN_180044d60() -> GameManager*
  /// @note FUN_180944040(target, playerId, amount): the engine's write point that lands the
  ///       combat-strength modifier into the player's bucket; playerId is the authoritative
  ///       player identity the engine itself resolved.
  void*   strengthAccumulate = nullptr;
  void*   changeYieldModifier = nullptr; ///< City::Instance::ChangeYieldModifier(YieldTypes, int)
  /// @note City yield read path (optional: when missing, the "percent per citizen" effect
  ///       degrades to no scaling).
  ///       City::Instance::CalculateYield(YieldTypes, TypeHash, bool) -> TrackedValue (sret).
  void*   cityCalculateYield = nullptr;
  /// @note TrackedValue::AddStep(this=modifier sub-object, step, u32=0, u32=0, tooltipKey):
  ///       modifier-detail append entry (step layout in civ6::YieldValue; the fifth argument
  ///       passes the localization key on the stack).
  void*   trackedValueAddStep = nullptr;
  /// @note AutoVariable persistence support (optional: when any of these is missing, the plugin's
  ///       persistence layer degrades to the current in-memory-only behavior).
  ///       City::Instance constructor: void*(void* self).
  void*   cityConstructor = nullptr;
  /// @brief Unit::Instance constructor: void*(void* self).
  void*   unitConstructor = nullptr;
  /// @note City/Unit destructors (optional, non-fatal: when either is missing, the corresponding
  ///       cleanup hook is skipped and side-table records are reclaimed only at context switches).
  ///       The plugin hooks them to erase its side-table records when the engine destroys an
  ///       object (city razing/capture, unit death/disband); the engine runs them during normal
  ///       gameplay and while tearing down the game context.
  /// @brief City::Instance destructor: void(void* self).
  void*   cityDestructor = nullptr;
  /// @brief Unit::Instance destructor: void(void* self).
  void*   unitDestructor = nullptr;
  /// @brief FAutoArchive variable registration helper:
  ///       void(void* variable, const void* name, void* archive).
  void*   autoVariableRegister = nullptr;
  /// @brief Engine aligned free wrapper: void(void* tag, void* pointer); frees the second argument.
  void*   engineAlignedFree = nullptr;
  /// @note AutoVariable archive traversal observation (optional, diagnostics only: when any of
  ///       these is missing, the corresponding observation hook is skipped and persistence and
  ///       gameplay are unaffected).
  /// @brief FAutoArchive vtable slot 1: void* (void* archive, void* variable) -> schema record.
  void*   autoVarLookupById = nullptr;
  /// @brief FAutoArchive vtable slot 2: void (void* archive, void* variable, const void* name).
  void*   autoVarRegisterById = nullptr;
  /// @brief Schema-container record creation: void* (void* container, const std::uint64_t* index).
  void*   autoVarRecordCreate = nullptr;
  /// @brief Writes the variable name into a schema record:
  ///       void* (void* record, const void* begin, const void* end).
  void*   autoVarRecordSetName = nullptr;
  /// @note AutoVariable descriptor value serialization (optional, diagnostics only: when any of
  ///       these is missing, the corresponding observation hook is skipped and persistence and
  ///       gameplay are unaffected). "Explicit" receives the stream as an argument; "Implicit"
  ///       recovers it from variable+0x08. Save/load direction is read from the traced caller.
  /// @brief Int descriptor value method taking the stream explicitly: void (void* variable, void* stream).
  void*   autoVarIntValueExplicit = nullptr;
  /// @brief Int descriptor value method recovering the stream from variable+0x08:
  ///       void* (void* variable) -> status.
  void*   autoVarIntValueImplicit = nullptr;
  /// @brief Int-array descriptor value method taking the stream explicitly:
  ///       void (void* variable, void* stream).
  void*   autoVarIntArrayValueExplicit = nullptr;
  /// @brief Int-array descriptor value method recovering the stream from variable+0x08:
  ///       void* (void* variable) -> status.
  void*   autoVarIntArrayValueImplicit = nullptr;
  /// @note Per-object City/Unit serialization (optional, non-fatal: when any of these is missing,
  ///       the plugin keeps its in-memory-only behavior). The engine's own serialize/deserialize
  ///       entry for a single City/Unit; the plugin hooks it to append its custom AutoVariable
  ///       values after the engine's own members on save and read them back on load.
  ///       Signature: void* (void* stream, void* object).
  void*   citySerializeSave = nullptr;
  void*   citySerializeLoad = nullptr;
  void*   unitSerializeSave = nullptr;
  void*   unitSerializeLoad = nullptr;
  /// @note Serialization read tracing (optional, diagnostics only: when missing, the corresponding
  ///       observation hook is skipped and gameplay/persistence are unaffected). The read trace
  ///       records the values these helpers transfer so a stream-derailment point can be located
  ///       from the crash dump (see serialization_trace.cpp).
  /// @brief Int-vector block serializer shared by save/load: it transfers a count through stream
  ///       vtable slot +0x28, then (key, value) pairs, writing the values into the data buffer.
  ///       Signature: std::uint64_t (void* stream, void* data, void* yieldKeyList). RVA 0x260A10.
  void*   autoVarIntArrayBlock = nullptr;
  /// @brief City yield int-vector loader (load-only). Signature:
  ///       void (void* stream, void* yieldVector, char flag). RVA 0x0296F0.
  void*   cityYieldIntVectorLoad = nullptr;
  /// @note Handler registration entries: handlerRegistryInit/setEffectHandler/handlerNodeInsert are
  ///       code. FUN_1804891b0(root) builds the built-in handler tables.
  void*   handlerRegistryInit = nullptr;
  void*   setEffectHandler = nullptr;  ///< FUN_1806083f0(root, kind, hash, handlerObj)
  void*   handlerNodeInsert = nullptr; ///< FUN_180489040(container, outNode, hashPtr)
  /// @note Build-profile data RVAs: not runtime-validated, must not be used for registration
  ///       (diagnostics only). They do not match across runs; the real objects are captured at
  ///       runtime from the engine registry via handlerNodeInsert (see effect_handler.cpp). These are
  ///       profile data describing the profiled template's handler, not evidence that the Loader
  ///       recognizes any particular effect.
  void*   profiledHandlerData = nullptr;  ///< profiled handler object (data)
  void*   profiledHandlerTable = nullptr; ///< its descriptor table (used to clone/replace apply)
  /// @note The profiled handler table's two slots (diagnostics only: to confirm whether a custom
  ///       effect reaches the handler apply path): slot 0: FUN_18046b0d0(self, args); slot 1:
  ///       FUN_18046b390(self, context, args).
  void*   profiledHandlerAnalyze = nullptr;
  void*   profiledHandlerApply = nullptr;
  /// @note handler dispatch thunk (RVA 0x979290): rcx=[rcx+0x18]; jmp [rax+0x30]. Optional
  ///       entry; when missing, only the corresponding hook is skipped (diagnostics + invalid
  ///       handler guard).
  void*   effectHandlerDispatch = nullptr;
  /// @note Enabled-mod query (optional, non-fatal). The engine keeps the live modding settings in an
  ///       in-memory object whose pointer lives in a global; the object is null until the engine
  ///       publishes the settings. enabledModsContainer is the address of that global (a data
  ///       pointer), getEnabledMods is the accessor FUN_180164870(container, out) that enumerates
  ///       the ENABLED_MODS collection into an engine vector of {const char* id, const char* title}
  ///       entries, and vectorDeallocate is the engine's std::vector release helper FUN_18009d470
  ///       used to free that vector. When any of them is missing the enabled-mod list is reported
  ///       as unavailable and cross-mod discovery degrades to the loader's own directory.
  void*   enabledModsContainer = nullptr; ///< Address of the settings-container global (data)
  void*   getEnabledMods = nullptr;       ///< FUN_180164870(container, out)
  void*   vectorDeallocate = nullptr;     ///< FUN_18009d470(vector, data, count)
};

/// @brief Get this DLL's directory.
/// @return Module directory path.
[[nodiscard]] std::wstring moduleDirectory();
/// @brief Ensure the GameCore module is loaded.
/// @return true on success.
[[nodiscard]] bool ensureGameCoreLoaded();
/// @brief Get the resolved GameCore entry-point set.
/// @return Reference to GameCoreApi.
[[nodiscard]] const GameCoreApi& gameCore();
/// @brief Engine entry-point set (populated after gameCore resolves successfully; fields are
///        null before resolution).
[[nodiscard]] const bridge::EngineApi& engineApi();

// gamecore.cpp: enabled-mod query (the engine's live in-memory modding settings).
/// @brief Read the engine's enabled-mod ids (a snapshot).
/// @param[out] out Receives the ids when the list is available; left unchanged otherwise.
/// @return true when the engine's enabled-mod list was read; false while it is not yet available
///         (the engine publishes the settings only after it has loaded them).
/// @note The first successful read is cached for the process lifetime.
[[nodiscard]] bool enabledModIds(std::vector<std::string>& out);

/// @brief Compute a string hash (same algorithm as the engine's MakeHash).
/// @param[in] text Input text.
/// @return 32-bit hash.
[[nodiscard]] std::uint32_t makeHash(const char* text);
/// @brief Emit one log line (narrow string).
/// @param[in] level Log level.
/// @param[in] message Text.
void logMessage(int level, const char* message);
/// @brief Emit one log line (wide string).
/// @param[in] level Log level.
/// @param[in] message Text.
void logMessage(int level, const std::wstring& message);
/// @brief Format a printf-style message and emit it to the log.
/// @param[in] level Log level.
/// @param[in] format Format string.
/// @param[in] ... Format arguments.
void logMessageF(int level, const char* format, ...);

// logging.cpp
/// @brief Initialize the log4cxx backend (idempotent). Must be called outside DllMain; the first
///        log call also triggers it, so even pre-initialization log lines reach the file.
void initializeLogging();
/// @brief Resolve the log file path (Known Folder Logs directory, falling back to the Loader
///        directory).
/// @return Absolute log file path, or an empty string when no directory could be resolved.
[[nodiscard]] std::wstring logFilePath();

// -- Level wrappers --
// Zero-overhead level macros. TRACE is never used; DEBUG is compiled out entirely unless
// _DEBUG is defined (so its formatting arguments are not evaluated). INFO and above always
// compile; the Loader's writer additionally applies the runtime YKKZ000_LOG_LEVEL filter.
#define logTrace(message) ((void)0)
#define logTraceF(...) ((void)0)
#if defined(_DEBUG)
#define logDebug(message) \
  logMessage(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kDebug), (message))
#define logDebugF(...) \
  logMessageF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kDebug), __VA_ARGS__)
#else
#define logDebug(message) ((void)0)
#define logDebugF(...) ((void)0)
#endif
#define logInfo(message) \
  logMessage(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kInfo), (message))
#define logInfoF(...) \
  logMessageF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kInfo), __VA_ARGS__)
#define logWarn(message) \
  logMessage(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kWarning), (message))
#define logWarnF(...) \
  logMessageF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kWarning), __VA_ARGS__)
#define logError(message) \
  logMessage(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kError), (message))
#define logErrorF(...) \
  logMessageF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kError), __VA_ARGS__)
#define logFatal(message) \
  logMessage(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kFatal), (message))
#define logFatalF(...) \
  logMessageF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kFatal), __VA_ARGS__)

/// @brief Stage log: the constructor emits "BEGIN: <stage>", the destructor emits "END: <stage>".
class LogScope {
 public:
  /// @brief Enter the stage; emits the BEGIN log.
  /// @param[in] stage Stage name (must stay valid for the LogScope lifetime).
  explicit LogScope(const char* stage) : stage_(stage) {
    logInfo((std::string("BEGIN: ") + stage_).c_str());
  }
  /// @brief Leave the stage; emits the END log.
  ~LogScope() { logInfo((std::string("END: ") + stage_).c_str()); }
  LogScope(const LogScope&) = delete;
  LogScope& operator=(const LogScope&) = delete;

 private:
  const char* stage_;
};

// registry.cpp
/// @brief Register one custom effect type.
/// @param[in] desc Effect descriptor (see bridge::EffectDesc).
/// @return 0 on success, non-zero on failure.
[[nodiscard]] int registerEffectType(const bridge::EffectDesc* desc);

/// @brief Record of a successfully registered EffectType, used for effect-handler registration.
/// @note typeName is this copy, so it does not rely on a plugin module that may already be
///       unloaded.
struct RegisteredEffect {
  std::uint32_t hash = 0;
  std::uint32_t templateHash = 0;
  std::string typeName;
};
/// @brief Get a snapshot of the registered effect records.
/// @return Vector of records.
[[nodiscard]] std::vector<RegisteredEffect> registeredEffects();
/// @brief Determine whether the given hash is a template-effect hash reused by some registered
///        custom effect.
/// @param[in] hash Hash to test.
/// @return true if it is a template hash.
/// @note Lets handler-node captures be recorded per template.
[[nodiscard]] bool isRegisteredTemplateHash(std::uint32_t hash);
/// @brief Determine whether the given hash is the type hash of some registered custom effect
///        itself (factory object +0x08).
/// @param[in] typeHash Hash to test.
/// @return true if it is a registered hash.
/// @note Used in template-Create bypass diagnostics to distinguish "engine built-in objects" from
///       "objects we cloned".
[[nodiscard]] bool isRegisteredHash(std::uint32_t typeHash);

/// @brief hook_service.cpp: initialization entry for the process-wide single MinHook instance
///        (idempotent).
/// @return true if initialized.
/// @note Shared by the Loader's own mechanism hooks and the plugin hook service.
[[nodiscard]] bool ensureHookServiceInitialized();

// hook_service.cpp: the Loader's own mechanism hooks (no ownership registration; not revoked
// when a plugin unloads).
/// @brief Install a Loader mechanism hook (no ownership registration).
/// @param[in] target Target address.
/// @param[in] detour Replacement function.
/// @param[out] original Receives the original function pointer.
/// @return 0 on success, non-zero on failure.
int installHookRaw(void* target, void* detour, void** original);
/// @brief Remove a Loader mechanism hook.
/// @param[in] target Target address.
/// @return 0 on success, non-zero on failure.
int removeHookRaw(void* target);

// hook_service.cpp: plugin hook service, ownership registered by pluginHandle.
/// @brief Plugin hook install service (ownership registered by pluginHandle).
/// @param[in] pluginHandle Owning plugin handle.
/// @param[in] target Target address.
/// @param[in] detour Replacement function.
/// @param[out] original Receives the original function pointer.
/// @return 0 on success, non-zero on failure.
int serviceInstallHook(void* pluginHandle, void* target, void* detour, void** original);
/// @brief Plugin hook remove service.
/// @param[in] pluginHandle Owning plugin handle.
/// @param[in] target Target address.
/// @return 0 on success, non-zero on failure.
int serviceRemoveHook(void* pluginHandle, void* target);
/// @brief Fallback revocation: remove every hook registered by this plugin that has not yet been
///        revoked.
/// @param[in] pluginHandle Owning plugin handle.
void removeHooksForPlugin(void* pluginHandle);
/// @brief Call-time batch: opened before registerEffectType calls the plugin prepare, closed
///        after the call.
void beginHookScope();
/// @brief Close the call-time batch.
/// @param[in] rollback When true, removes every hook newly created within the batch.
/// @note Fallback for a failed prepare; the primary responsibility remains the plugin revoking
///       its own hooks.
void endHookScope(bool rollback);

// memory_probe.cpp
/// @brief Determine whether [address, address+bytes) falls within committed, readable memory.
/// @param[in] address Start address.
/// @param[in] bytes Length.
/// @return true if readable.
[[nodiscard]] bool isReadableRegion(const void* address, std::size_t bytes);
/// @brief Determine whether a pointer looks like a dereferenceable object (not a low address,
///        8-byte aligned, first pointer readable).
/// @param[in] pointer Pointer to test.
/// @return true if it looks like a candidate object.
[[nodiscard]] bool isCandidateObject(const void* pointer);

/// @brief Read a field only after isReadableRegion validates it.
/// @tparam T Field type.
/// @param[in] base Base address.
/// @param[in] offset Field offset.
/// @param[out] out Receives the read result.
/// @return true on success; false on failure without modifying out.
template <typename T>
[[nodiscard]] bool tryReadField(const void* base, std::size_t offset, T& out) {
  if (base == nullptr) {
    return false;
  }
  const auto* address = static_cast<const std::uint8_t*>(base) + offset;
  if (!isReadableRegion(address, sizeof(T))) {
    return false;
  }
  std::memcpy(&out, address, sizeof(T));
  return true;
}

// -- Member access layer: translate "member references" into offsets --
// Call sites express fields with (&civ6::X::field); the offset is decided by the layout in
// <ykkz000/civ6/*.h>. Engine pointers are still validated with isReadableRegion first; when that
// fails, memory is left untouched.

/// @brief Get a field's offset from a member pointer.
/// @tparam TObj Object type.
/// @tparam TField Field type.
/// @param[in] member Member pointer.
/// @return Byte offset of the field relative to the object start.
/// @note Takes the member address against an aligned static dummy object, avoiding taking the
///       address of a null pointer; only address arithmetic is performed, no member is read.
template <class TObj, class TField>
[[nodiscard]] std::size_t MemberOffset(TField TObj::* member) {
  static const TObj kDummy{};
  const auto base = reinterpret_cast<std::uintptr_t>(&kDummy);
  const auto field = reinterpret_cast<std::uintptr_t>(&(kDummy.*member));
  return static_cast<std::size_t>(field - base);
}

/// @brief Read a member field after validating readability.
/// @tparam TObj Object type.
/// @tparam TField Field type.
/// @param[in] base Object base address.
/// @param[in] member Member pointer.
/// @param[out] out Receives the read result.
/// @return true on success; false on failure without modifying out.
template <class TObj, class TField>
[[nodiscard]] bool TryRead(const void* base, TField TObj::* member, TField& out) {
  return tryReadField(base, MemberOffset(member), out);
}

/// @brief Read a member field after validating readability, returning fallback on failure.
/// @tparam TObj Object type.
/// @tparam TField Field type.
/// @param[in] base Object base address.
/// @param[in] member Member pointer.
/// @param[in] fallback Value returned when the read fails.
/// @return Field value or fallback.
template <class TObj, class TField>
[[nodiscard]] TField TryReadOr(const void* base, TField TObj::* member, TField fallback) {
  TField value = fallback;
  (void)TryRead(base, member, value);
  return value;
}

/// @brief Write a member field after validating readability.
/// @tparam TObj Object type.
/// @tparam TField Field type.
/// @param[in] base Object base address.
/// @param[in] member Member pointer.
/// @param[in] value Value to write.
/// @return true on success; false on failure without modifying memory.
template <class TObj, class TField>
bool TryWrite(void* base, TField TObj::* member, const TField& value) {
  if (base == nullptr) {
    return false;
  }
  auto* address = static_cast<std::uint8_t*>(base) + MemberOffset(member);
  if (!isReadableRegion(address, sizeof(TField))) {
    return false;
  }
  std::memcpy(address, &value, sizeof(TField));
  return true;
}

// crash_capture.cpp
/// @brief Install crash-context capture (VEH); idempotent.
void installCrashCapture();
/// @brief Uninstall crash capture: remove the VEH so it cannot point at unloaded code after the
///        DLL is unloaded.
void uninstallCrashCapture();

/// @brief Whether this thread is currently executing an SEH-guarded engine/plugin call.
/// @note The VEH passes straight through (EXCEPTION_CONTINUE_SEARCH) when it sees this flag,
///       letting the protected call's __except take over and avoiding writing recoverable,
///       probing exceptions into YKKZ000_crash.log as fatal crashes.
extern thread_local bool g_guardedCallActive;

// effect_handler.cpp
/// @brief Install the effect-handler hooks.
/// @return true on success.
[[nodiscard]] bool installEffectHandlerHook();
/// @brief Uninstall the effect-handler hooks.
void uninstallEffectHandlerHook();
/// @brief Remove the handler nodes this mod registered.
/// @note Call before forwarding to the real DllDestroyGameContext (when root is still valid).
///       Do not call at any other time.
void removeCustomEffectHandlers();
/// @brief Unload fallback: clear the analyze/handlerApply callbacks this plugin registered in the
///        handler clone tables.
/// @param[in] pluginHandle Owning plugin handle.
/// @note Prevents a handler from jumping into unloaded memory after the plugin is unloaded.
void clearHandlerCallbacksForPlugin(void* pluginHandle);

// vtable_clone.cpp
/// @brief Clone the template factory vtable.
/// @param[in] templateFactory Template factory object.
/// @return The cloned vtable pointer.
[[nodiscard]] void* cloneFactoryVTable(void* templateFactory);
/// @brief Remember the type name for a type hash.
/// @param[in] typeHash Type hash.
/// @param[in] typeName Type name (an internal copy is kept).
void rememberTypeName(std::uint32_t typeHash, const char* typeName);
/// @brief Registration-failure rollback: remove the type-name record for a hash.
/// @param[in] typeHash Type hash.
/// @note The GetTypeName slot then returns an empty string.
void forgetTypeName(std::uint32_t typeHash);

// effect_mechanism.cpp
/// @brief A registered plugin implementation.
/// @note impl is a copy and carries the owning handle; originalCreate is the template factory
///       Create.
struct EffectRecord {
  bridge::EffectImpl impl{};
  void* originalCreate = nullptr;
  void* pluginHandle = nullptr;
};
/// @brief Get the implementation record for a custom EffectType hash.
/// @param[in] typeHash Type hash.
/// @param[out] out Receives the record on a hit.
/// @return true if present, otherwise false.
[[nodiscard]] bool findEffectRecord(std::uint32_t typeHash, EffectRecord& out);
/// @brief Register/refresh the implementation and template Create for an EffectType (repeated
///        calls with the same hash refresh it).
/// @param[in] typeHash Type hash.
/// @param[in] impl Plugin implementation.
/// @param[in] originalCreate Template factory Create.
/// @return 0 on success, non-zero on failure.
int registerEffectImpl(std::uint32_t typeHash, const bridge::EffectImpl* impl,
                       void* originalCreate);
/// @brief Registration-failure rollback: remove the implementation record for an EffectType.
/// @param[in] typeHash Type hash.
/// @return 0 on success, non-zero (not found).
int unregisterEffectImpl(std::uint32_t typeHash);
/// @brief Clone the effect object's vtable and replace its Apply/Remove slots with the
///        template/impl function pointers from the record.
/// @param[in] effectObject Effect object.
/// @param[in] typeHash Type hash.
/// @return The patched effect object.
[[nodiscard]] void* patchEffectObjectSlots(void* effectObject, std::uint32_t typeHash);
/// @brief Custom factory Create entry.
/// @return The Create function pointer.
[[nodiscard]] void* customFactoryCreateEntry();
/// @brief Unload fallback: restore the effect-object slots this plugin replaced to the template
///        functions and clear its implementation callback pointers.
/// @param[in] pluginHandle Owning plugin handle.
void teardownPluginEffects(void* pluginHandle);

// plugin_manager.cpp
/// @brief Load all plugins.
/// @param[in] host Host service table.
void loadPlugins(bridge::Host* host);
/// @brief Unload all plugins.
void unloadPlugins();
/// @brief Set the handle of the plugin currently being loaded.
/// @param[in] pluginHandle Owning plugin handle.
/// @note Only set during a GetPlugin call, so registration can attribute ownership.
void setActivePluginHandle(void* pluginHandle);
/// @brief Get the handle of the plugin currently being loaded.
/// @return The owning plugin handle.
[[nodiscard]] void* activePluginHandle();
/// @brief Broadcast a context lifecycle event to all loaded plugins.
/// @param[in] event Event type.
/// @param[in] context Context pointer.
void notifyPluginsGameContext(bridge::GameContextEvent event, void* context);

// proxy.cpp
/// @brief Initialize the Loader idempotently.
void initializeLoaderOnce();
/// @brief Whether Loader initialization has completed.
/// @return true if initialized.
[[nodiscard]] bool loaderInitialized();
/// @brief Create the game context.
/// @return Context pointer.
[[nodiscard]] void* createGameContext();
/// @brief Destroy the game context.
/// @param[in] context Context pointer.
void destroyGameContext(void* context);
/// @brief Get this session's telemetry hash.
/// @return Session hash.
[[nodiscard]] std::uint64_t telemetrySessionHash();

} // namespace ykkz000::loader
