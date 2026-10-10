#pragma once

#include <cstddef>
#include <cstdint>

/// @file host.h
/// @brief Pure C ABI boundary definitions between the host (loader) and plugins.
/// @note Uses only PODs, function pointers, and const char*; no std:: objects or exceptions are
///       passed across the DLL boundary.
namespace ykkz000::bridge {

/// @brief Host API version number.
/// @note Evolves under an "append-only" policy: new fields may only be appended, and the meaning
///       of existing fields never changes; plugins use this to decide whether the host supports
///       the capabilities they need.
inline constexpr std::uint32_t kHostApiVersion = 17;

struct Host;

/// @brief String hash: same algorithm as the engine's GameCore::Utilities::MakeHash.
using MakeHashFn          = std::uint32_t (*)(const char*);
/// @brief Log output: level is the log level, msg is UTF-8 text.
using LogFn               = void (*)(int level, const char* msg);
/// @brief Get the effect registry handle.
using GetEffectRegistryFn = void* (*)();

/// @note Added in v17 (append-only): engine-enabled-mod enumeration.
/// @brief Number of mods the engine has enabled for this session.
/// @return Count, or 0 when the engine has not yet published the enabled-mod list.
using GetEnabledModCountFn = std::uint32_t (*)();
/// @brief Copy the UTF-8 id (the .modinfo UUID) of one enabled mod into a caller buffer.
/// @param[in] index Zero-based enabled-mod index.
/// @param[out] buffer Destination buffer.
/// @param[in] capacity Buffer size in bytes.
/// @return Bytes written excluding the terminator, or 0 when the index is out of range, the buffer
///         is too small, or the enabled-mod list is unavailable.
using GetEnabledModIdFn = std::uint32_t (*)(std::uint32_t index, char* buffer,
                                            std::uint32_t capacity);

/// @brief Uniform signature for the Apply/Remove slots of an effect object.
/// @note The engine passes arguments according to the template's real signature; here they are
///       uniformly received as 4 pointer arguments (receiving extra arguments is harmless on x64),
///       and forwarded with the same arguments.
using ApplyFn = std::uint64_t (*)(void* self, void* a1, void* a2, void* a3);
/// @brief Signature for handler descriptor-table slot 0 (analyze) and slot 1 (apply).
using AnalyzeFn = void* (*)(void* self, void* args);

/// @brief The "implementation" a plugin provides for a custom EffectType.
/// @note All slots are optional: a null slot keeps the template behavior; the loader handles
///       everything generically and no longer recognizes any specific behavior.
struct EffectImpl {
  /// @brief Engine template Apply function pointer.
  /// @note The loader uses it (by function-pointer value) to locate the slot to replace in the
  ///       cloned vtable; it must be provided when apply/remove are non-null.
  const void* templateApply;
  /// @brief Engine template Remove function pointer (same semantics as templateApply).
  const void* templateRemove;
  ApplyFn     apply;        ///< Replaces the effect object's Apply slot; null = keep the template
  ApplyFn     remove;       ///< Replaces the effect object's Remove slot; null = keep the template
  AnalyzeFn   analyze;      ///< Replaces handler slot 0; null = forward to the template
  ApplyFn     handlerApply; ///< Replaces handler slot 1; null = forward to the template
  const char* label;        ///< Diagnostic name (may be null)
  void*       userData;     ///< Plugin context; the loader keeps it as-is and does not interpret it
};

/// @note GameEffects metadata is written by the engine via the template's GetTypeInfo; custom
///       metadata is not supported under the current mechanism, so no corresponding field is
///       provided here.

/// @brief Prepare entry point (hook installation function).
/// @param[in] userData The plugin context passed through as-is at registration time (see
///   EffectDesc::userData).
/// @return 0 on success; non-zero on failure.
/// @note Contract:
///       * Called once by host->registerEffectType after the factory/type have been registered
///         (registration phase).
///       * If it returns non-zero, registerEffectType rolls back this registration (the factory
///         object/type/impl record) and returns the error code; prepare itself must undo the hooks
///         it installed this time (the loader does not perform a half-way rollback).
///       * May be null (this effect needs no hook).
///       * Context-related re-installation/deactivation is not done here: it still goes through
///         Host::onGameContext (kCreated/kDestroyed) and DestroyPlugin (the plugin shuts itself
///         down).
using EffectPrepareFn = int (*)(void* userData);

/// @brief Describes a custom effect type to be registered.
struct EffectDesc {
  const char* typeName;       ///< Required: custom EffectType name
  const char* templateEffect; ///< Required: name of an existing effect whose behavior and parameter definitions are reused
  const EffectImpl* impl;     ///< null = fully reuse the template behavior
  EffectPrepareFn prepare;    ///< Called by registerEffectType; null = no hook needed
  void* userData;             ///< Passed to prepare as-is
};

/// @brief Register a custom effect type.
/// @return 0 on success, non-zero on failure.
using RegisterEffectTypeFn = int (*)(const EffectDesc*);

/// @brief Engine entry points: a read-only set of function pointers.
/// @note Evolves under an "append-only" policy: existing fields never change meaning.
struct EngineApi {
  void* effectApply;          ///< Effects::AdjustCityYieldModifier::Apply/Remove
  void* effectRemove;
  void* effectStrengthApply;  ///< Effects::AdjustPlayerStrengthModifier::Apply/Remove
  void* effectStrengthRemove;
  void* proposedCombatAdjust; ///< Combat-strength-modifier write point (authoritative source of the player id)
  void* changeYieldModifier;  ///< City::Instance::ChangeYieldModifier(YieldType, int)
  void* getPlayer;            ///< PlayerTypes -> Player::Instance* (no bounds check)
  void* getGameManager;       ///< -> GameManager* (+0x50 is the player vector)

  /// @note Added in v7 (append-only).
  /// @brief City yield read path: City::Instance::CalculateYield(YieldTypes, TypeHash, bool).
  /// @note Returns a TrackedValue (hidden sret: rcx=sret, rdx=city, r8=yield, r9=typeHash,
  ///       stack=flag). The base accumulator is at sret+0x10, the modifier sub-object at sret+0x30
  ///       (the modifier accumulator is at sret+0x40).
  void* cityCalculateYield;

  /// @note Added in v8 (append-only).
  /// @brief Modifier-detail append entry: TrackedValue::AddStep(this=modifier sub-object (out+0x30),
  ///       step, u32=0, u32=0, const char* tooltipKey).
  /// @note step is layout-identical to the sub-object (civ6::YieldValue: has_min/min/has_max/max/
  ///       value/flag/steps, 0x30 bytes); the engine writes only value and zeroes the remaining
  ///       fields.
  void* trackedValueAddStep;

  /// @note Added in v10 (append-only): AutoVariable persistence support.
  /// @brief City::Instance constructor (registers the engine's AutoVariable members on its
  ///       FAutoArchive at instance+0x08). Signature: void*(void* self).
  void* cityConstructor;
  /// @brief Unit::Instance constructor (same AutoVariable registration pattern).
  ///       Signature: void*(void* self).
  void* unitConstructor;
  /// @brief FAutoArchive variable registration helper.
  ///       Signature: void(void* variable, const void* name_object, void* archive), where
  ///       name_object points to a 24-byte engine name object {void* buffer; const char* begin;
  ///       const char* end;}. The buffer is a NUL-terminated engine-aligned allocation that the
  ///       helper copies during the call, so the caller may release it afterwards with
  ///       engineAlignedFree (exactly like the engine's own constructors). The helper appends the
  ///       variable to the archive and registers its name; the caller sets the variable's
  ///       vtable/value afterwards.
  void* autoVariableRegister;
  /// @brief Engine aligned allocator: void*(std::size_t size). The returned block is compatible
  ///       with engineAlignedFree and with the engine's own containers, so it can be used for the
  ///       storage of a custom AutoVariable.
  void* engineAlignedMalloc;
  /// @brief Engine aligned free wrapper: void(void* tag, void* pointer); frees the second argument
  ///       with the engine's _aligned_free. The first argument is ignored by the engine wrapper
  ///       (pass nullptr).
  void* engineAlignedFree;

  /// @note Added in v11 (append-only): AutoVariable archive traversal observation (diagnostics
  ///       only; the plugin uses these solely to attribute the save/load traversal).
  /// @brief FAutoArchive vtable slot 1: looks up the schema record for a variable by its index in
  ///       the archive's variable vector. Signature: void* (void* archive, void* variable);
  ///       returns the record pointer or null when absent.
  void* autoVarLookupById;
  /// @brief FAutoArchive vtable slot 2: ensures a schema record exists for the variable's index and
  ///       writes its name. Signature: void (void* archive, void* variable,
  ///       const void* name_object), where name_object is a 24-byte engine name object.
  void* autoVarRegisterById;
  /// @brief Schema-container record creation. Signature: void* (void* container,
  ///       const std::uint64_t* index); returns the new record.
  void* autoVarRecordCreate;
  /// @brief Writes the variable name into a schema record. Signature: void* (void* record,
  ///       const void* begin, const void* end).
  void* autoVarRecordSetName;

  /// @note Added in v12 (append-only): AutoVariable descriptor value serialization (diagnostics
  ///       only). The engine serializes a variable's value through its descriptor vtable; these are
  ///       the int scalar and int-array descriptor value methods the trace hooks instrument. The
  ///       "Explicit" method receives the stream/archive as an argument; the "Implicit" method
  ///       recovers it from variable+0x08. Which of the pair runs on save versus load is not
  ///       assumed statically and is read from the traced caller in the log. They are reached only
  ///       from the engine's save/load traversal, never from gameplay logic.
  /// @brief Int descriptor value method that takes the stream explicitly. Signature:
  ///       void (void* variable, void* stream).
  void* autoVarIntValueExplicit;
  /// @brief Int descriptor value method that recovers the stream from variable+0x08. Signature:
  ///       void* (void* variable) -> status.
  void* autoVarIntValueImplicit;
  /// @brief Int-array descriptor value method that takes the stream explicitly. Signature:
  ///       void (void* variable, void* stream).
  void* autoVarIntArrayValueExplicit;
  /// @brief Int-array descriptor value method that recovers the stream from variable+0x08.
  ///       Signature: void* (void* variable) -> status.
  void* autoVarIntArrayValueImplicit;

  /// @note Added in v13 (append-only): per-object City/Unit serialization. The engine's own
  ///       serialize/deserialize entry for a single object; the plugin hooks it, forwards the call
  ///       unchanged, and then appends its custom AutoVariable values through the variable's own
  ///       descriptor vtable (save slot +0x20, load slot +0x08), so the values travel in the same
  ///       stream as the engine's own members. Signature: void* (void* stream, void* object).
  void* citySerializeSave;
  void* citySerializeLoad;
  void* unitSerializeSave;
  void* unitSerializeLoad;

  /// @note Added in v14 (append-only): City/Unit destructor entry points. The plugin hooks them to
  ///       erase its side-table records when the engine destroys an object (city razing/capture,
  ///       unit death/disband). The engine runs these destructors during normal gameplay and while
  ///       tearing down the game context, so the hooks also observe teardown destruction as long as
  ///       they are removed only after the real context destroy.
  /// @brief City::Instance destructor: void(void* city).
  void* cityDestructor;
  /// @brief Unit::Instance destructor: void(void* unit).
  void* unitDestructor;

  /// @note Added in v15 (append-only): serialization read-trace entries (diagnostics only; consumed
  ///       by the Loader's own _DEBUG serialization trace). Plugins do not need them and may ignore
  ///       them; the fields are resolved on the Loader side regardless.
  /// @brief Int-vector block serializer shared by save/load. It transfers a count through stream
  ///       vtable slot +0x28, then (key, value) pairs, writing values into the data buffer.
  ///       Signature: std::uint64_t (void* stream, void* data, void* yieldKeyList). RVA 0x260A10.
  void* autoVarIntArrayBlock;
  /// @brief City yield int-vector loader (load-only). Signature:
  ///       void (void* stream, void* yieldVector, char flag). RVA 0x0296F0.
  void* cityYieldIntVectorLoad;

  /// @note Added in v17 (append-only): the engine's live enabled-mod list, read from the in-memory
  ///       modding settings. A plugin can use it to gate content on which other mods are active.
  ///       The list is a snapshot taken from the engine; when the engine has not yet published the
  ///       settings, the count is 0 and no id can be read.
  /// @brief Number of mods currently enabled by the engine in this session.
  GetEnabledModCountFn getEnabledModCount;
  /// @brief Copy the index-th enabled mod's id into buffer (see GetEnabledModIdFn).
  GetEnabledModIdFn    getEnabledModId;
};

/// @brief Install a hook.
/// @param[in] pluginHandle The plugin-ownership handle allocated by the host.
/// @param[in] target The target address.
/// @param[in] detour The replacement function.
/// @param[out] original Receives the original (trampoline) function pointer.
/// @return 0 means "the target is taken over by this plugin and usable" (including a repeated call
///   for an already-installed hook).
/// @note The single process-wide MinHook instance is owned by the loader's hook_service. Plugins
///       must not link MinHook themselves (two instances would corrupt each other's trampolines);
///       all installation/removal goes through here. Idempotent: it may be called repeatedly during
///       registration and on every game-context creation; a repeated call writes the existing
///       trampoline back into original, which the plugin can use to tell that the hook is still
///       valid. Switching the same target to a different detour is rejected (non-zero).
using HookInstallFn = int (*)(void* pluginHandle, void* target, void* detour, void** original);
/// @brief Remove a hook.
/// @param[in] pluginHandle The plugin-ownership handle allocated by the host.
/// @param[in] target The target address.
/// @return 0 means "the target is no longer taken over by this plugin" (including a repeated call
///   for an already-removed/never-registered hook).
/// @note "Already installed/already removed" are success semantics and must never be treated as a
///       failure -- otherwise the plugin would clear a trampoline that is still in effect.
using HookRemoveFn  = int (*)(void* pluginHandle, void* target);

/// @brief Validated memory read: expressed as "offset + byte count" to avoid passing type
///   information across the DLL boundary.
/// @param[in] base Base address.
/// @param[in] offset Field offset.
/// @param[in] bytes Number of bytes to read.
/// @param[out] out Receives the read result.
/// @return 1 on success; 0 on failure without modifying out.
using ReadFieldFn  = int (*)(const void* base, std::size_t offset, std::size_t bytes, void* out);
/// @brief Validated memory write (same semantics as ReadFieldFn).
/// @param[in] in Data to write; returns 0 on failure without modifying in.
using WriteFieldFn = int (*)(void* base, std::size_t offset, std::size_t bytes, const void* in);

/// @brief Determine whether a pointer is a valid engine-object candidate.
using IsCandidateFn = int (*)(const void* pointer);
/// @brief Determine whether [address, address+bytes) is readable.
using IsReadableFn  = int (*)(const void* address, std::size_t bytes);

/// @brief Game-context lifecycle events.
enum class GameContextEvent : std::int32_t {
  kCreated = 0,   ///< New game context created
  kDestroyed = 1, ///< Game context destroyed
};

/// @brief Context lifecycle notification.
/// @param[in] event kCreated or kDestroyed.
/// @param[in] context The context pointer provided by the host.
/// @note On kCreated the plugin installs and enables its own hooks and clears caches that became
///       invalid with the context; on kDestroyed it disables its hooks and cleans up caches. The
///       callback is wrapped in SEH by the loader.
using ContextListenerFn = void (*)(GameContextEvent event, void* context);

/// @brief The set of services the host provides to plugins.
/// @note Fields evolve under an "append-only" policy; apiVersion is used for version gating (see
///       kHostApiVersion).
struct Host {
  std::uint32_t        apiVersion;         ///< Host API version (see kHostApiVersion)
  MakeHashFn           makeHash;           ///< String hash
  RegisterEffectTypeFn registerEffectType; ///< Register a custom effect type
  LogFn                log;                ///< Log output
  void*                gameCoreModule;     ///< GameCore module base address
  GetEffectRegistryFn  getEffectRegistry;  ///< Get the effect registry

  /// @note Added in v5 (append-only).
  const EngineApi*     engine;             ///< Engine entry-point table
  HookInstallFn        installHook;        ///< Install a hook (idempotent)
  HookRemoveFn         removeHook;         ///< Remove a hook
  ReadFieldFn          readField;          ///< Validated memory read
  WriteFieldFn         writeField;         ///< Validated memory write
  IsCandidateFn        isCandidateObject;  ///< Engine-object candidate test
  IsReadableFn         isReadableRegion;   ///< Readable-region test
  ContextListenerFn    onGameContext;      ///< Context lifecycle notification
  void*                pluginHandle;       ///< Ownership handle the loader allocates for each plugin
};

/// @brief Plugin export entry point: called by the loader after loading the plugin to inject the
///   Host.
/// @param[in] host The host service table.
/// @return 0 on success, non-zero on failure.
using GetPluginFn     = int  (*)(Host* host);
/// @brief Plugin unload entry point: called by the loader before unloading the plugin to release
///   resources.
using DestroyPluginFn = void (*)();

/// @brief Plugin manifest returned by the GetPluginManifest export.
/// @note A POD whose storage (the name string and the dependency-name array) is owned by the plugin
///       module and must stay valid for as long as the module is loaded. The loader reads it before
///       any GetPlugin call to build the dependency graph and order initialization.
struct PluginManifest {
  std::uint32_t structSize;        ///< sizeof(PluginManifest) as seen by the plugin build
  std::uint32_t apiVersion;        ///< Host API version the plugin was built against
  const char* name;                ///< Unique logical plugin id (required; never null/empty)
  std::uint32_t versionMajor;      ///< Plugin version major (informational)
  std::uint32_t versionMinor;      ///< Plugin version minor (informational)
  std::uint32_t versionPatch;      ///< Plugin version patch (informational)
  const char* const* dependencies; ///< Dependency plugin names (may be null when none)
  std::uint32_t dependencyCount;   ///< Number of dependency names in dependencies
  std::uint32_t requiredHostApi;   ///< Minimum host API version required by this plugin
};

/// @brief Plugin manifest export entry point: called by the loader before GetPlugin to build the
///   dependency graph.
/// @return Pointer to the plugin's resident manifest; the module keeps it alive.
using GetPluginManifestFn = const PluginManifest* (*)();

} // namespace ykkz000::bridge

/// @brief Plugin export name "GetPlugin".
#define YKKZ000_PLUGIN_EXPORT_GETPLUGIN  "GetPlugin"
/// @brief Plugin export name "DestroyPlugin".
#define YKKZ000_PLUGIN_EXPORT_DESTROY    "DestroyPlugin"
/// @brief Plugin export name "GetPluginManifest".
#define YKKZ000_PLUGIN_EXPORT_GETPLUGINMANIFEST "GetPluginManifest"
