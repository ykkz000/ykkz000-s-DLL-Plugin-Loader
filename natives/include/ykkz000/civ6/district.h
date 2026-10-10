#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

/// @file district.h
/// @brief Layout mirror of the engine's GameCore::District::Instance.
/// @note This mod uses only its pointer and does not read business fields; offsets pinned by
///       static_assert.
namespace ykkz000::civ6 {

/// @brief Engine GameCore::District::Instance (this mod uses only its pointer and does not read
///   business fields).
struct District {
  struct Instance {
    void** vtable;                     ///< 0x000: District object vtable
    std::uint8_t unknown_0x008[0xb8];  ///< 0x008..0x0BF: Unknown
    /// @brief 0x0C0: PlotCoord (8 bytes, two int32).
    /// @note Evidence: District::Instance::GetOwner looks up the map with this+0xC0 and then reads
    ///       the plot owner (the char at Plot+0x1C).
    std::uint8_t plot_coord[0x8];
  };
};

static_assert(std::is_standard_layout_v<District::Instance>);
static_assert(offsetof(District::Instance, plot_coord) == 0xc0);
static_assert(sizeof(District::Instance::plot_coord) == 0x8);

} // namespace ykkz000::civ6
