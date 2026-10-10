#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/common.h>

/// @file factory.h
/// @brief Layout mirror of the engine's IModifierEffectFactory registry entry.
/// @note Describes layout only; offsets pinned by static_assert.
namespace ykkz000::civ6 {

/// @brief Entry object of Registry<IModifierEffectFactory>.
/// @note Allocated by Platform::MallocTemp when a custom effect is registered; see registry.cpp.
struct ModifierEffectFactory {
  void** vtable;                ///< 0x00: Factory vtable (see the kFactory*Slot constants in common.h)
  std::uint32_t type_hash;      ///< 0x08: EffectType hash; equivalent to GetTypeId()'s return value
  std::uint8_t unknown_0x0c[4]; ///< 0x0C: Unknown (alignment padding)
};

static_assert(std::is_standard_layout_v<ModifierEffectFactory>);
static_assert(offsetof(ModifierEffectFactory, type_hash) == 0x08);
static_assert(sizeof(ModifierEffectFactory) == 0x10);

} // namespace ykkz000::civ6
