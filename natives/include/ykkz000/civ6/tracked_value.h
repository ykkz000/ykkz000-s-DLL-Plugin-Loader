#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/common.h>

/// @file tracked_value.h
/// @brief GameCore engine object layout mirror: TrackedValue (the yield accumulator structure of
///   "base value + modifier steps").
/// @note City::Instance::CalculateYield(YieldTypes, TypeHash, bool) returns this structure via a
///       hidden sret: rcx = return object, rdx = city, r8 = yield, r9 = typeHash, stack =
///       record_steps.
/// @note Layout evidence (decompiled item by item from the release image, 2026-09-29):
///       * CalculateYield (0x12FF20) initializes the return object as two isomorphic 0x30-byte
///         sub-objects: +0x00 has_min=0 / +0x08 has_max=0 / +0x10 value=0 / +0x14 record_steps /
///         +0x18 detail vector; the modifier sub-object (+0x30) additionally sets +0x30 has_min to
///         1 and +0x34 min to 0xFFFF9C00 (-25600 == -100% lower bound).
///       * The detail step AddStep (0x12FC10) merges one modifier into this+0x10, where this is the
///         "modifier sub-object"; its step parameter is isomorphic to the sub-object above (the
///         convenience overload 0x12FCE0 constructs it all-zero: has_min/has_max/flag set to 0,
///         only value written).
///       * The constant 0x100 represents +1 percentage point (FixedPoint<8>): +100% == 25600.
///       * Combination (release FUN_180076460):
///         final = clamp(base) + clamp(base) * clamp(modifier) / 25600.
///       * The read path (this mod's side table) converts to modifier units via
///         (percent x population) >> 8 and appends through AddStep, fully isomorphic to the
///         engine's own modifier sources (game effects / religion / governor titles, ...).
namespace ykkz000::civ6 {

/// @brief Engine "fixed-point value with lower/upper bounds + detail steps" structure
///   (FixedPoint<8>, 256 == +1%).
/// @note Reading clamps down to the upper bound first (when has_max is non-zero,
///       value = min(value, max)), then up to the lower bound (when has_min is non-zero,
///       value = max(value, min)); when both switches are 0, the raw value is taken. As the step
///       parameter of AddStep, the engine writes only value and zeroes the remaining fields.
struct YieldValue {
  bool has_min;                 ///< 0x00: Non-zero enables lower-bound clamping
  std::int32_t min;             ///< 0x04: Lower bound (FixedPoint<8>)
  bool has_max;                 ///< 0x08: Non-zero enables upper-bound clamping
  std::int32_t max;             ///< 0x0c: Upper bound (FixedPoint<8>)
  std::int32_t value;           ///< 0x10: Accumulator/step (FixedPoint<8>, 256 == +1%)
  bool flag;                    ///< 0x14: Detail-recording switch (the engine passes 0 when used as a step)
  std::uint8_t unknown_0x15[3]; ///< 0x15..0x17: Unknown/padding
  VectorView<void*> steps;      ///< 0x18: Detail vector (begin/end/cap)
};

static_assert(std::is_standard_layout_v<YieldValue>);
static_assert(offsetof(YieldValue, has_min) == 0x00);
static_assert(offsetof(YieldValue, min) == 0x04);
static_assert(offsetof(YieldValue, has_max) == 0x08);
static_assert(offsetof(YieldValue, max) == 0x0c);
static_assert(offsetof(YieldValue, value) == 0x10);
static_assert(offsetof(YieldValue, flag) == 0x14);
static_assert(offsetof(YieldValue, steps) == 0x18);
static_assert(sizeof(YieldValue) == 0x30);

/// @brief Return object of CalculateYield: the base accumulator sub-object (+0x00) and the
///   modifier sub-object (+0x30).
/// @note base.value is the engine's "base yield", modifier.value is the accumulated percentage
///       modifier (25600 == +100%); modifier.min is pre-set to -25600 by CalculateYield.
struct TrackedValue {
  YieldValue base;     ///< 0x00: Base accumulator (base.value is the base yield)
  YieldValue modifier; ///< 0x30: Modifier sub-object (modifier.value is the modifier accumulator; the this of AddStep)
};

static_assert(std::is_standard_layout_v<TrackedValue>);
static_assert(offsetof(TrackedValue, base) == 0x00);
static_assert(offsetof(TrackedValue, modifier) == 0x30);
static_assert(sizeof(TrackedValue) == 0x60);

} // namespace ykkz000::civ6
