#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// @file mods_sqlite.h
/// @brief Read-only access to the engine's Mods.sqlite scan/cache database.
/// @note The module only turns the database file into data. It reads a whole-file snapshot and
///       parses the SQLite structures itself (no external dependency); when the database is in WAL
///       mode or the snapshot parse fails, the system's winsqlite3.dll serves the same queries
///       read-only as a fallback. The enabled-mod set is the selected mod group: the ModGroups row
///       with Selected set, joined through ModGroupItems to the non-disabled Mods rows, whose
///       .modinfo paths come from ScannedFiles. Nothing is written, no engine state is read, and
///       no result is cached across calls: the caller decides when to re-read, so a rewritten
///       database can be picked up. Every failure (missing file, lock, WAL without winsqlite3,
///       unusable format, no selected group) is a data condition, never a Loader failure: the
///       caller then treats the enabled-mod set as unavailable.
namespace ykkz000::loader {

/// @brief The enabled-mod data read from Mods.sqlite's selected group.
struct ModsSqliteSnapshot {
  /// @brief Whether the database was read and a selected group was found (all other fields valid).
  bool ok = false;
  /// @brief Whether the winsqlite3.dll fallback served the read instead of the snapshot parser.
  bool usedWinSqlite = false;
  /// @brief File change counter from the database header (diagnostics only).
  std::uint32_t fileChangeCounter = 0;
  /// @brief ModGroups.ModGroupRowId of the selected group.
  std::int64_t selectedGroupRowId = 0;
  /// @brief ModGroups.Name of the selected group (a localization key for the default group).
  std::string selectedGroupName;
  /// @brief Selected group rows including disabled ones (diagnostics only).
  std::size_t groupMemberCount = 0;
  /// @brief Non-disabled member mod ids, ASCII-lowercased, deduplicated and sorted.
  std::vector<std::string> enabledModIds;
  /// @brief The .modinfo path of each enabled member that has one, as stored by the scan engine
  ///        (absolute, or relative to the game executable's directory), paired with the same
  ///        lowercase mod id. Mods without a readable path are omitted.
  std::vector<std::pair<std::string, std::wstring>> idToModinfoPath;
};

/// @brief Read the enabled mods of Mods.sqlite's selected group.
/// @param[out] out Receives the snapshot on success; left unchanged (ok false) on failure.
/// @return true when the database was read and a selected group with members was found; false when
///         the file is missing, locked, in a format the reader cannot use, or carries no selected
///         group (the caller then treats the enabled-mod set as unavailable).
[[nodiscard]] bool readModsSqlite(ModsSqliteSnapshot& out);

/// @brief Resolve the engine's Mods.sqlite path (LocalAppData Known Folder).
/// @return Absolute database path, or an empty string when the Known Folder cannot be resolved.
[[nodiscard]] std::wstring modsSqlitePath();

} // namespace ykkz000::loader
