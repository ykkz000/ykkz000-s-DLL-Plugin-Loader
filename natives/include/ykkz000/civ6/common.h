#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

/// @file common.h
/// @brief Common definitions for the GameCore engine's runtime memory layout.
/// @note The layout was confirmed by reverse engineering GameCore_XP2_FinalRelease.dll. This
///       directory describes layout only: types contain only POD data members (void*, fixed-width
///       integers, fixed-length arrays), with no virtual functions, base classes, or non-trivial
///       members, and call no engine functions; each known field pins its offset and size with a
///       static_assert. Hard-coded offsets and vtable slot numbers on the loader side must be
///       changed to reference the types here (offsetof / sizeof / slot constants), so the engine
///       layout has a single source of truth.
namespace ykkz000::civ6 {

/// @brief Upper bound on the yield vector.
/// @note DLC/mods can change the actual number of yields, so this is validated at runtime by
///       taking the minimum against the engine's real length, instead of hard-coding the yield
///       count of a particular build. Used for fixed-length arrays indexed by YieldTypes, such as
///       extra::CityExtra.
/// @note Directory semantics: civ6/ mirrors the engine layout only; extra/ holds this mod's
///       overlay data and side tables.
inline constexpr std::size_t kMaxYields = 64;

/// @brief Number of MSVC vtable prefix slots.
/// @note vtable[-1] is the RTTI/COL pointer and must be copied along when cloning a vtable.
///       Otherwise the memory in front of the new vptr is a HeapAlloc block header, and the
///       engine's dynamic_cast/typeid/exception unwinding would treat that header as a pointer and
///       corrupt memory.
inline constexpr std::size_t kVTableRttiPrefixSlots = 1;

/// @brief Vtable width of an IModifierEffectFactory object.
/// @note The interface is wider than 6 slots: in practice it dispatches on slot 6 (+0x30), so a
///       clone must be "wide enough and fully copied" and must never copy only 6 slots, or it will
///       read past the end into heap garbage and jump to 0xFFFFFFFF.
inline constexpr std::size_t kFactoryVTableSlots = 24;
/// @brief GetTypeId slot in the factory vtable.
inline constexpr std::size_t kFactoryTypeIdSlot = 1;
/// @brief GetTypeName slot in the factory vtable.
inline constexpr std::size_t kFactoryTypeNameSlot = 2;
/// @brief Create slot in the factory vtable.
inline constexpr std::size_t kFactoryCreateSlot = 5;

/// @brief Three-pointer std::vector view (MSVC layout).
template <typename T>
struct VectorView {
  T* begin;    ///< 0x00: First element
  T* end;      ///< 0x08: One past the last element
  T* capacity; ///< 0x10: End of capacity
};

static_assert(std::is_standard_layout_v<VectorView<void*>>);
static_assert(offsetof(VectorView<void*>, begin) == 0x00);
static_assert(offsetof(VectorView<void*>, end) == 0x08);
static_assert(offsetof(VectorView<void*>, capacity) == 0x10);
static_assert(sizeof(VectorView<void*>) == 0x18);

/// @brief std::shared_ptr view (MSVC layout).
/// @note In the construction context of FUN_18092A4F0, a "pointer field + its control block at
///       +0x08" appear in pairs, i.e. a shared_ptr.
template <typename T>
struct SharedPtrView {
  T* pointer;          ///< 0x00: Object pointer
  void* control_block; ///< 0x08: Control block
};

static_assert(std::is_standard_layout_v<SharedPtrView<void>>);
static_assert(offsetof(SharedPtrView<void>, pointer) == 0x00);
static_assert(offsetof(SharedPtrView<void>, control_block) == 0x08);
static_assert(sizeof(SharedPtrView<void>) == 0x10);

} // namespace ykkz000::civ6
