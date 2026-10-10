#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/district.h>
#include <ykkz000/civ6/unit.h>

/// @file combat.h
/// @brief Layout mirror of the combat-strength-modifier structures in the engine's GameEffects
///   namespace.
/// @note Describes layout only; offsets pinned by static_assert.
namespace ykkz000::civ6 {

/// @brief Engine GameEffects namespace.
struct GameEffects {
  /// @brief Engine GameEffects::ProposedCombat: the combat-strength-modifier accounting structure,
  ///   i.e. the this pointer of the write-point function
  ///   AdjustPlayerStrengthModifier(PlayerTypes, int).
  /// @note Accounting rules (full decompilation of AdjustPlayerStrengthModifier):
  ///       - unit at +0x00 is non-null and unit+0x128 == playerType => +0x2C += amount;
  ///       - otherwise district at +0x08 is non-null and District::Instance::GetOwner() ==
  ///         playerType => +0x2C += amount;
  ///       - neither matches => +0x30 += amount.
  struct ProposedCombat {
    Unit::Instance* unit;              ///< 0x00: Target unit (non-null means account to that unit specifically)
    District::Instance* district;      ///< 0x08: Target district (non-null means account to the district's owner)
    std::uint8_t unknown_0x10[0x1c];   ///< 0x10..0x2B: Unknown
    std::int32_t directed_total;       ///< 0x2C: Directed unit/district accumulator
    std::int32_t general_total;        ///< 0x30: General accumulator (no target unit/district matched)
  };
};

static_assert(std::is_standard_layout_v<GameEffects::ProposedCombat>);
static_assert(offsetof(GameEffects::ProposedCombat, unit) == 0x00);
static_assert(offsetof(GameEffects::ProposedCombat, district) == 0x08);
static_assert(offsetof(GameEffects::ProposedCombat, directed_total) == 0x2c);
static_assert(offsetof(GameEffects::ProposedCombat, general_total) == 0x30);

} // namespace ykkz000::civ6
