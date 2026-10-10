#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

/// @file city.h
/// @brief Layout mirror of the engine's GameCore::City::Instance and its yield cache.
/// @note Describes layout only (offsets pinned by static_assert) and contains no mod overlay data;
///       overlay data lives in extra/.
namespace ykkz000::civ6 {

/// @brief Engine city yield-cache entry (the data `City::Instance::yield_cache` points to; each
///   entry is 8 bytes and indexed by YieldType).
/// @note Evidence (release image FUN_1801332F0, i.e. the caller of CalculateYield):
///       entry = *(longlong*)(city + 0x1950) + yield * 8;
///       if (*(char*)(entry + 4) == 0) { CalculateYield(...); *entry = combine(...);
///                                       *(char*)(entry + 4) = 1; }  // recompute and mark valid
///       *out = *entry;                                              // hit: reuse the old value
///       i.e. it recomputes only when valid == 0, and returns the cached value directly when
///       valid != 0. Changes to this mod's side table (overlay data) do not clear this flag, so
///       Apply/Remove must clear valid themselves to force a recompute.
struct YieldCacheEntry {
  std::int32_t value;      ///< 0x00: Cached value (written back when the engine recomputes)
  std::uint8_t valid;      ///< 0x04: Valid flag (0 = the next read must recompute)
  std::uint8_t pad_0x05[3];
};

static_assert(sizeof(YieldCacheEntry) == 8);

/// @brief Engine GameCore::City::Instance.
/// @note Describes layout only and contains no mod overlay data (overlay data lives in extra/).
///       Each known field pins its offset with a static_assert; the comments give "offset +
///       meaning + evidence function".
struct City {
  struct Instance {
    void** vtable;                      ///< 0x000: City object vtable
    std::uint8_t unknown_0x008[0xa0];   ///< 0x008..0xA7: Unknown
    /// @brief 0xA8: City ID (the serial-number part of Player::CityID).
    /// @note Evidence: Exports::City::GetID (*(uint*)(instance+0xA8)), Lua::Utility::GetCityID
    ///       (*(u32*)(city+0xA8), which also reads the owner player as a short at city+0xD8).
    ///       After a city object is destroyed its pointer may be recycled, so this ID identifies
    ///       whether it is still the same city.
    ///       City::Instance::ChangePopulation/ChangeYieldModifier read it when assembling a
    ///       notification.
    std::int32_t city_id;
    std::uint8_t unknown_0x0ac[0x2c];   ///< 0x0AC..0x0D7: Unknown
    /// @brief 0xD8: PlayerTypes -- the player that owns the city.
    /// @note Evidence: City::Instance::CalculateYield reads the player via *(u32*)(city+0xD8)
    ///       (e.g. when feeding Context::Globals::GetPlayer and Player::Governors::Get);
    ///       ChangePopulation/ChangeYieldModifier take the low 16 bits here when assembling a
    ///       CityID; serialization also reads it here.
    std::int32_t owner;
    std::uint8_t unknown_0x0dc[0x18c];  ///< 0x0DC..0x267: Unknown
    /// @brief 0x268: int (population).
    /// @note Evidence: Player::Stats::GetPopulation walks the city chain and accumulates
    ///       city+0x268; Lua's GetPopulation also reads this offset; ChangePopulation guards with
    ///       `0 < *(int*)(city+0x268)+delta`.
    std::int32_t population;
    std::uint8_t unknown_0x26c[0x234];  ///< 0x26C..0x49F: Unknown

    /// @note Game-effects city yield modifiers (starting at 0x4B0).
    ///       Evidence: FUN_180131cf0 (City::Instance::ChangeYieldModifier) computes
    ///       v = FUN_18072a920(city+0x4A0); disassembly confirms FUN_18072a920 returns
    ///       param_1+0x10, so v = city+0x4B0: v[0](+0x00)=int32 array pointer, v[2](+0x10)=count,
    ///       v+0x18=out-of-range fallback scalar;
    ///       `yield < count ? ptr[yield] += amount : fallback += amount`.
    ///       Note: earlier documentation wrote +0x4B0 directly as the "array start"; this round
    ///       corrects it to the offsets below.
    std::uint8_t unknown_0x4a0[0x10];   ///< 0x4A0..0x4AF: Modifier-container header (+0x08 is the tree-root pointer)
    std::int32_t* yield_modifier_game_effects_data;    ///< 0x4B0: Modifier array (indexed by YieldTypes)
    std::uint8_t unknown_0x4b8[8];                     ///< 0x4B8..0x4BF: Unknown
    std::int32_t yield_modifier_game_effects_count;    ///< 0x4C0: Count (read as (int)qword)
    std::uint8_t unknown_0x4c4[4];                     ///< 0x4C4..0x4C7: High 32 bits within the slot
    std::int32_t yield_modifier_game_effects_fallback; ///< 0x4C8: Fallback accumulator for out-of-range/no-match cases
    std::uint8_t unknown_0x4cc[0x44];   ///< 0x4CC..0x50F: Unknown

    /// @note Per-governor-title yield modifiers (starting at 0x510).
    ///       Evidence: CalculateYield (0x12FF20) block at 0x180130A50 x Governor::GetNumTitles;
    ///       the tooltip string LOC_CITY_YIELD_MODIFIER_PER_GOVERNOR_TITLE_TOOLTIP
    ///       (0x1809CAC68) is referenced at 0x180130AFB/0x180130B19 in that function.
    ///       Offsets are recorded from the isomorphic layout and were not individually
    ///       disassembly-verified this round.
    std::int32_t* yield_modifier_per_governor_title_data;    ///< 0x510
    std::uint8_t unknown_0x518[8];                           ///< 0x518..0x51F: Unknown
    std::int32_t yield_modifier_per_governor_title_count;    ///< 0x520
    std::uint8_t unknown_0x524[4];                           ///< 0x524..0x527
    std::int32_t yield_modifier_per_governor_title_fallback; ///< 0x528
    std::uint8_t unknown_0x52c[0x14];   ///< 0x52C..0x53F: Unknown

    /// @note Flat yield array (starting at 0x540, semantics unverified).
    ///       Evidence: the block at 0x1801306C7 in CalculateYield multiplies this array by 0x100
    ///       and adds it into the base accumulator +0x10.
    std::int32_t* flat_yields_0x540_data;    ///< 0x540
    std::uint8_t unknown_0x548[8];           ///< 0x548..0x54F: Unknown
    std::int32_t flat_yields_0x540_count;    ///< 0x550
    std::uint8_t unknown_0x554[4];           ///< 0x554..0x557
    std::int32_t flat_yields_0x540_fallback; ///< 0x558
    std::uint8_t unknown_0x55c[0x134];  ///< 0x55C..0x68F: Unknown

    /// @note Per-population yield array (starting at 0x690, unit FixedPoint<8>).
    ///       Evidence: City::Instance::GetYieldFromPopulation (0x133780, called from CalculateYield
    ///       at 0x180130013): iVar5 x count x 0x100 accumulation.
    ///       Offsets are recorded from the isomorphic layout and were not individually
    ///       disassembly-verified this round.
    std::int32_t* per_population_yields_data;    ///< 0x690
    std::uint8_t unknown_0x698[8];               ///< 0x698..0x69F: Unknown
    std::int32_t per_population_yields_count;    ///< 0x6A0
    std::uint8_t unknown_0x6a4[4];               ///< 0x6A4..0x6A7
    std::int32_t per_population_yields_fallback; ///< 0x6A8
    std::uint8_t unknown_0x6ac[4];               ///< 0x6AC..0x6AF: Tail padding

    /// @note City yield cache (0x1950).
    ///       Evidence (release image FUN_1801332F0): entry = *(longlong*)(city+0x1950) + yield*8;
    ///       valid is at entry+4; when valid == 0 the engine calls CalculateYield to recompute,
    ///       writes value back (+0) and sets the flag to 1, otherwise it returns the cached value
    ///       directly. Side-table changes must clear valid (see the YieldCacheEntry comment).
    ///       When FUN_180951550()+0x2A0 == 0 the engine is in "no cache" mode (always recomputes),
    ///       in which case invalidation is harmless.
    std::uint8_t unknown_0x6b0[0x1950 - 0x6b0]; ///< 0x6B0..0x194F: Unknown
    void* yield_cache;                          ///< 0x1950: YieldCacheEntry vector data
  };
};

static_assert(std::is_standard_layout_v<City::Instance>);
static_assert(offsetof(City::Instance, city_id) == 0xa8);
static_assert(offsetof(City::Instance, owner) == 0xd8);
static_assert(offsetof(City::Instance, population) == 0x268);
static_assert(offsetof(City::Instance, yield_modifier_game_effects_data) == 0x4b0);
static_assert(offsetof(City::Instance, yield_modifier_game_effects_count) == 0x4c0);
static_assert(offsetof(City::Instance, yield_modifier_game_effects_fallback) == 0x4c8);
static_assert(offsetof(City::Instance, yield_modifier_per_governor_title_data) == 0x510);
static_assert(offsetof(City::Instance, yield_modifier_per_governor_title_count) == 0x520);
static_assert(offsetof(City::Instance, yield_modifier_per_governor_title_fallback) == 0x528);
static_assert(offsetof(City::Instance, flat_yields_0x540_data) == 0x540);
static_assert(offsetof(City::Instance, flat_yields_0x540_count) == 0x550);
static_assert(offsetof(City::Instance, flat_yields_0x540_fallback) == 0x558);
static_assert(offsetof(City::Instance, per_population_yields_data) == 0x690);
static_assert(offsetof(City::Instance, per_population_yields_count) == 0x6a0);
static_assert(offsetof(City::Instance, per_population_yields_fallback) == 0x6a8);
static_assert(offsetof(City::Instance, yield_cache) == 0x1950);

} // namespace ykkz000::civ6
