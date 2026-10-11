#pragma once

#include <cstddef>
#include <string>
#include <vector>

/// @file enabled_mods_build.h
/// @brief Reusable ENABLED_MODS construction and read-back capability.
/// @note Builds the engine's modding-settings ENABLED_MODS section from scratch with the engine's
///       own object constructors and map/vector-map write primitives, and reads the published
///       collection back through the engine's own accessor. All engine calls are SEH-guarded, so a
///       faulting call degrades to a false result instead of taking down the process. The module is
///       not gated on _DEBUG; callers decide when to use it.
namespace ykkz000::loader {

/// @brief One enabled-mod entry used as builder input.
/// @note The strings are borrowed views: they only need to stay valid for the duration of the
///       buildAndInsertEnabledMods call, which copies them into process-lifetime storage.
struct EnabledMod {
  const char* id = nullptr;    ///< Mod id (for example the .modinfo UUID).
  const char* title = nullptr; ///< Display title.
};

/// @brief One enabled-mod entry read back from the engine (owned copies).
struct EnabledModEntry {
  /// @brief Mod id.
  std::string id;
  /// @brief Display title (empty when the engine entry carries none).
  std::string title;
};

/// @brief Build an ENABLED_MODS section from `mods` and insert it into the live configuration
///        manager under the ENABLED_MODS key.
/// @param[in] manager The configuration manager to insert into (the settings map at
///            *(container + 0x88)); it must satisfy the engine's candidate-object test.
/// @param[in] mods Entry array; entries may carry a null title (treated as empty), and an entry
///            with a null or empty id is skipped.
/// @param[in] count Number of entries.
/// @return true when the section was built and inserted; false when a required engine entry point
///         is missing, the ENABLED_MODS key already exists (an existing section is never
///         overwritten), no usable entry was supplied, or a construction step raised (the engine
///         state is then left untouched).
/// @note The object graph is built with the engine's own constructors (real vtables), so the bag's
///       field getter and the accessor read it like an engine-built section. The input strings are
///       copied into process-lifetime storage, because the engine keeps pointer views for tag-5
///       string variants and never frees them. Must run on the game thread while the settings are
///       published, and only when the section is intentionally supplied by the Loader: an inserted
///       section pre-empts the engine's own later population, which never overwrites it.
[[nodiscard]] bool buildAndInsertEnabledMods(void* manager, const EnabledMod* mods,
                                             std::size_t count);

/// @brief Enumerate the engine's published ENABLED_MODS collection through its own accessor.
/// @param[out] out Receives owned copies of the entries on success; left unchanged on failure.
/// @return true when the published settings container was available and enumerated (out may then be
///         empty while the engine has not populated the collection yet); false when the container
///         or the accessor is unavailable, or the engine call raised.
/// @note The engine's accessor-allocated vector is released with the engine's own deallocator; the
///       copies in out do not depend on it.
[[nodiscard]] bool enumerateEnabledMods(std::vector<EnabledModEntry>& out);

} // namespace ykkz000::loader
