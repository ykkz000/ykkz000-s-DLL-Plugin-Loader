#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/player.h>

/// @file unit.h
/// @brief Layout mirror of the engine's GameCore::Unit::Instance.
/// @note Describes layout only; offsets pinned by static_assert.
namespace ykkz000::civ6 {

/// @brief Engine GameCore::Unit::Instance.
/// @note Unit identity: +0xB0 is the unit ID (the release image FUN_18005c4a0 prints
///       "Player %i Unit %i ..." with `*(int*)(unit+0xB0)`; FUN_1803a48d0 compares
///       `*(int*)(unit+0xB0)` item by item against IDs in the unit's list). The same player's
///       UnitManager (Player+0x6C8) looks up a unit by that ID via
///       `FUN_1802c4360(manager, unitId)` (the low 16 bits are the slot), so (owner, unit_id) is a
///       stable key within a player; slots are recycled, so the read side must still re-read to
///       verify.
struct Unit {
  struct Instance {
    void** vtable;                      ///< 0x000: Unit object vtable
    std::uint8_t unknown_0x008[0xa8];   ///< 0x008..0x0AF: Unknown
    std::int32_t unit_id;               ///< 0x0B0: Unit ID (uint32; key of Player::UnitManager)
    std::uint8_t unknown_0x0b4[0x74];   ///< 0x0B4..0x127: Unknown
    /// @brief 0x128: PlayerTypes (owner).
    /// @note Evidence: GameEffects::ProposedCombat::AdjustPlayerStrengthModifier:
    ///       *(PlayerTypes*)(unit+0x128) == playerType.
    PlayerTypes owner;
  };
};

static_assert(std::is_standard_layout_v<Unit::Instance>);
static_assert(offsetof(Unit::Instance, unit_id) == 0xb0);
static_assert(offsetof(Unit::Instance, owner) == 0x128);

} // namespace ykkz000::civ6
