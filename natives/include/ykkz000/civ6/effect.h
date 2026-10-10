#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/player.h>
#include <ykkz000/civ6/unit.h>

/// @file effect.h
/// @brief Layout mirror of the engine's effect objects and handler arguments (city yield / player
///   combat-strength modifier).
/// @note Describes layout only; offsets pinned by static_assert.
namespace ykkz000::civ6 {

/// @brief Runtime object of Effects::AdjustCityYieldModifier (city yield modifier effect).
/// @note Unlike the player-strength template, it stores its parameters as two parallel arrays
///       (YieldType[] / Amount[]) and records "entry count" and "amount" separately at +0x18 /
///       +0x30. The cloned vtable locates and replaces the Apply/Remove slots by function pointer
///       (see the loader's effect_mechanism.cpp).
struct CityYieldModifierEffect {
  void** vtable;                ///< 0x00: Effect object vtable
  int* yields;                  ///< 0x08: YieldType array
  std::uint8_t unknown_0x10[8]; ///< 0x10: Unknown (documented as the array capacity)
  std::int32_t entry_count;     ///< 0x18: Entry count
  std::uint8_t unknown_0x1c[4]; ///< 0x1C: Unknown
  int* amounts;                 ///< 0x20: Amount array
  std::uint8_t unknown_0x28[8]; ///< 0x28: Unknown (documented as the array capacity)
  std::int32_t amount_count;    ///< 0x30: Amount
  std::uint8_t unknown_0x34[4]; ///< 0x34: Unknown
};

static_assert(std::is_standard_layout_v<CityYieldModifierEffect>);
static_assert(offsetof(CityYieldModifierEffect, yields) == 0x08);
static_assert(offsetof(CityYieldModifierEffect, entry_count) == 0x18);
static_assert(offsetof(CityYieldModifierEffect, amounts) == 0x20);
static_assert(offsetof(CityYieldModifierEffect, amount_count) == 0x30);

/// @brief Runtime object of Effects::AdjustPlayerStrengthModifier (player combat-strength modifier
///   effect).
/// @note Unlike the city-yield template, this effect does not use arrays but stores its parameters
///       as scalars: +0x40 Amount (written by FUN_1808d5cf0 after parsing "Amount"). Verified
///       semantics of Apply (FUN_1808D9110):
///       - when +0x44 != 100, stack exponentially via powf (the stacking count is maintained by
///         FUN_180951d20);
///       - +0x48 is the cap;
///       - +0x4C is the scope (domain; -1 means disabled);
///       - +0x50 is a per-mille scalar (multiplied by 0x6400 on application);
///       - +0x54 is a percentage scaled by turn/era (FUN_18063d0e0/FUN_18063d160);
///       - +0x58 is scaled by player count (reads GameManager+0x10D0/+0x10D8);
///       - the result accumulates into +0x5C (total applied), and the player identity comes from
///         ownerObj+0xD8.
///       Remove (FUN_1808DA240) rolls back exactly by +0x5C.
///       Note: field names follow the offset-based naming convention; the exact semantics of a few
///       names remain "unverified".
struct AdjustPlayerStrengthModifier {
  void** vtable;                               ///< 0x00: Effect object vtable
  std::uint8_t unknown_0x08[0x38];             ///< 0x08..0x3F: Unknown
  std::int32_t amount;                         ///< 0x40: Amount (parsed from "Amount")
  std::int32_t stack_percent_0x44;             ///< 0x44: Exponential-style StackPercent (!=100 uses powf)
  std::int32_t cap_0x48;                       ///< 0x48: Cap/inverse scaling
  std::int32_t domain_0x4c;                    ///< 0x4C: Scope domain (-1 disables)
  std::int32_t scalar_0x50;                    ///< 0x50: Per-mille scalar (multiplied by 0x6400 on application)
  std::int32_t advanced_start_multiplier_0x54; ///< 0x54: Percentage scaled by turn/era
  std::int32_t scale_by_player_count_0x58;     ///< 0x58: Scaled by player count
  std::int32_t applied_total;                  ///< 0x5C: Total applied (source for preview and exact rollback)
};

static_assert(std::is_standard_layout_v<AdjustPlayerStrengthModifier>);
static_assert(offsetof(AdjustPlayerStrengthModifier, amount) == 0x40);
static_assert(offsetof(AdjustPlayerStrengthModifier, stack_percent_0x44) == 0x44);
static_assert(offsetof(AdjustPlayerStrengthModifier, cap_0x48) == 0x48);
static_assert(offsetof(AdjustPlayerStrengthModifier, domain_0x4c) == 0x4c);
static_assert(offsetof(AdjustPlayerStrengthModifier, scalar_0x50) == 0x50);
static_assert(offsetof(AdjustPlayerStrengthModifier, advanced_start_multiplier_0x54) == 0x54);
static_assert(offsetof(AdjustPlayerStrengthModifier, scale_by_player_count_0x58) == 0x58);
static_assert(offsetof(AdjustPlayerStrengthModifier, applied_total) == 0x5c);
static_assert(sizeof(AdjustPlayerStrengthModifier) == 0x60);

/// @brief Arguments of the template handler's apply slot (slot 1).
/// @note Confirmed by decompiling FUN_18046b390.
struct EffectArgs {
  std::uint8_t unknown_0x00[8]; ///< 0x00: Unknown
  std::int32_t amount;          ///< 0x08: Amount
  std::int32_t yield_type;      ///< 0x0C: YieldType
};

static_assert(std::is_standard_layout_v<EffectArgs>);
static_assert(offsetof(EffectArgs, amount) == 0x08);
static_assert(offsetof(EffectArgs, yield_type) == 0x0c);
static_assert(sizeof(EffectArgs) == 0x10);

/// @brief Effect application context.
/// @note The City at +0x70 is the field actually used by the city-yield template (FUN_180464310);
///       the Player/Unit at +0x80/+0x90 are only documented from an old guess in FUN_180465AD0 and
///       are not used by the loader.
struct EffectContext {
  std::uint8_t unknown_0x00[0x70]; ///< 0x00..0x6F: Unknown
  City::Instance* city;            ///< 0x70: City instance
  std::uint8_t unknown_0x78[8];    ///< 0x78: Unknown
  Player::Instance* player;        ///< 0x80: Player (unverified)
  std::uint8_t unknown_0x88[8];    ///< 0x88: Unknown
  Unit::Instance* unit;            ///< 0x90: Unit (unverified)
};

static_assert(std::is_standard_layout_v<EffectContext>);
static_assert(offsetof(EffectContext, city) == 0x70);
static_assert(offsetof(EffectContext, player) == 0x80);
static_assert(offsetof(EffectContext, unit) == 0x90);

} // namespace ykkz000::civ6
