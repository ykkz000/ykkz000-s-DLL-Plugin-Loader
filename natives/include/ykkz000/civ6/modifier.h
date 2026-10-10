#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/common.h>

/// @file modifier.h
/// @brief Layout mirror of the per-modifier application runtime context and the effect-subject
///   wrapper.
/// @note Describes layout only; offsets pinned by static_assert.
namespace ykkz000::civ6 {

/// @brief Per-modifier application runtime context (a DynamicModifier instance).
/// @note Constructed by FUN_18092A4F0; the first argument of the driver entry FUN_180944D60 is
///       this instance: +0x148 (+0x150 is the control block) = the owner value (constructor
///       parameter param_3); +0x1B8 (+0x1C0 is the control block) = the effect object (constructor
///       parameter param_6, i.e. the object with the vtable patch applied). After verifying
///       ownership with ctx+0x1B8 == self, ctx+0x148 is the host of this effect object (any engine
///       object carrying a player ID, not necessarily a Player::Instance*; first resolve it using
///       the Player::Instance::player_type offset). The remaining fields only record known
///       offsets: entries marked "unverified" are tentative types and do not participate in loader
///       behavior.
struct DynamicModifierContext {
  void** vtable;                        ///< 0x00: Context vtable
  std::uint8_t unknown_0x08[0xe0];      ///< 0x08..0xE7: Unknown
  std::uint32_t effect_type_0xe8;       ///< 0xE8: effect_type (type unverified)
  std::uint8_t unknown_0xec[0x14];      ///< 0xEC..0xFF: Unknown
  std::uint8_t active_flag_0x100;       ///< 0x100: active flag (type unverified)
  std::uint8_t unknown_0x101[0x2f];     ///< 0x101..0x12F: Unknown
  std::uint8_t disabled_flag_0x130;     ///< 0x130: disabled flag (type unverified)
  std::uint8_t unknown_0x131[0x7];      ///< 0x131..0x137: Unknown
  SharedPtrView<void> owner_property;   ///< 0x138: owner_property (probably a shared_ptr)
  void* owner;                          ///< 0x148: owner value (host object)
  void* owner_control_0x150;            ///< 0x150: owner control block
  SharedPtrView<void> definition_property; ///< 0x158: definition_property
  void* collection_provider_0x168;      ///< 0x168: collection provider (unverified)
  std::uint8_t unknown_0x170[0x8];      ///< 0x170..0x177: Unknown
  SharedPtrView<void> subjects_property; ///< 0x178: subjects_property
  std::uint8_t unknown_0x188[0x20];     ///< 0x188..0x1A7: Unknown
  void* subject_provider_0x1a8;         ///< 0x1A8: subject provider (unverified)
  std::uint8_t unknown_0x1b0[0x8];      ///< 0x1B0..0x1B7: Unknown
  void* effect_object;                  ///< 0x1B8: Effect object (instance with the vtable patch applied)
  void* effect_object_control_0x1c0;    ///< 0x1C0: Control block of the effect object
  std::uint8_t unknown_0x1c8[0x10];     ///< 0x1C8..0x1D7: Unknown
  std::int32_t reentry_depth_0x1d8;     ///< 0x1D8: Re-entry depth (type unverified)
  std::uint8_t unknown_0x1dc[0x24];     ///< 0x1DC..0x1FF: Unknown
  void* subject_collection_0x200;       ///< 0x200: subject collection (unverified)
};

static_assert(std::is_standard_layout_v<DynamicModifierContext>);
static_assert(offsetof(DynamicModifierContext, effect_type_0xe8) == 0xe8);
static_assert(offsetof(DynamicModifierContext, active_flag_0x100) == 0x100);
static_assert(offsetof(DynamicModifierContext, disabled_flag_0x130) == 0x130);
static_assert(offsetof(DynamicModifierContext, owner_property) == 0x138);
static_assert(offsetof(DynamicModifierContext, owner) == 0x148);
static_assert(offsetof(DynamicModifierContext, owner_control_0x150) == 0x150);
static_assert(offsetof(DynamicModifierContext, definition_property) == 0x158);
static_assert(offsetof(DynamicModifierContext, collection_provider_0x168) == 0x168);
static_assert(offsetof(DynamicModifierContext, subjects_property) == 0x178);
static_assert(offsetof(DynamicModifierContext, subject_provider_0x1a8) == 0x1a8);
static_assert(offsetof(DynamicModifierContext, effect_object) == 0x1b8);
static_assert(offsetof(DynamicModifierContext, effect_object_control_0x1c0) == 0x1c0);
static_assert(offsetof(DynamicModifierContext, reentry_depth_0x1d8) == 0x1d8);
static_assert(offsetof(DynamicModifierContext, subject_collection_0x200) == 0x200);

/// @brief The a1 argument of the effect object's Apply slot is a subject wrapper.
/// @note Calling convention per FUN_180944D60: a1+0x00 is the subject object itself. The remaining
///       fields are recorded by offset only (corroborated by FUN_180949820 / FUN_180943CD0); types
///       are unverified.
struct EffectSubjectWrapper {
  void* object;                         ///< 0x00: The subject object itself
  std::uint8_t unknown_0x08[0x8];       ///< 0x08..0x0F: Unknown
  std::uint16_t flags_0x10;             ///< 0x10: Flag bits (type unverified)
  std::uint8_t unknown_0x12[0xe];       ///< 0x12..0x1F: Unknown
  SharedPtrView<void> collection_0x20;  ///< 0x20: Collection (shared pointer)
  std::uint8_t unknown_0x30[0x10];      ///< 0x30..0x3F: Unknown
  std::uint8_t flag_0x40;               ///< 0x40: Flag (type unverified)
  std::uint8_t flag_0x41;               ///< 0x41: Flag (type unverified)
};

static_assert(std::is_standard_layout_v<EffectSubjectWrapper>);
static_assert(offsetof(EffectSubjectWrapper, object) == 0x00);
static_assert(offsetof(EffectSubjectWrapper, flags_0x10) == 0x10);
static_assert(offsetof(EffectSubjectWrapper, collection_0x20) == 0x20);
static_assert(offsetof(EffectSubjectWrapper, flag_0x40) == 0x40);
static_assert(offsetof(EffectSubjectWrapper, flag_0x41) == 0x41);

} // namespace ykkz000::civ6
