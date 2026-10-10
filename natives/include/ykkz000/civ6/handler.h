#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

/// @file handler.h
/// @brief Layout mirror of the engine's effect-handler descriptor table, object, and registration
///   hash table.
/// @note Describes layout only; offsets pinned by static_assert.
namespace ykkz000::civ6 {

/// @brief Descriptor table of an effect handler: an array of function pointers.
/// @note Slot 0 = analyze, slot 1 = apply (confirmed by the template implementation and runtime
///       logs); slot 6 (+0x30) is tail-called by dispatch thunk 0x979290
///       (rcx=[rcx+0x18]; jmp [rax+0x30]).
struct HandlerTable {
  void* (*analyze)(void* self, void* args);                     ///< 0x00: Slot 0
  std::uint64_t (*apply)(void* self, void* context, void* args); ///< 0x08: Slot 1
  void* unknown_0x10;                                           ///< 0x10: Slot 2 (unverified)
  void* unknown_0x18;                                           ///< 0x18: Slot 3 (unverified)
  std::uint64_t (*release)(void* self, int flags);              ///< 0x20: Slot 4
  void* unknown_0x28;                                           ///< 0x28: Slot 5 (unverified)
  void* (*dispatch_slot)(void* handler);                        ///< 0x30: Slot 6
};

static_assert(std::is_standard_layout_v<HandlerTable>);
static_assert(offsetof(HandlerTable, analyze) == 0x00);
static_assert(offsetof(HandlerTable, apply) == 0x08);
static_assert(offsetof(HandlerTable, release) == 0x20);
static_assert(offsetof(HandlerTable, dispatch_slot) == 0x30);
static_assert(sizeof(HandlerTable) == 0x38);

/// @brief Descriptor-table slots (derived from the field offsets to stay consistent with the
///   layout).
inline constexpr std::size_t kHandlerAnalyzeSlot = offsetof(HandlerTable, analyze) / sizeof(void*);
inline constexpr std::size_t kHandlerApplySlot = offsetof(HandlerTable, apply) / sizeof(void*);
inline constexpr std::size_t kHandlerReleaseSlot = offsetof(HandlerTable, release) / sizeof(void*);

static_assert(kHandlerAnalyzeSlot == 0);
static_assert(kHandlerApplySlot == 1);
static_assert(kHandlerReleaseSlot == 4);

/// @brief A handler object is a single-pointer object: handler[0] points to the descriptor table.
/// @note A self-built object must be zeroed and allocated with enough space (0x40 bytes), or the
///       remaining slots will read heap garbage (the root cause of the exit crash).
struct HandlerObject {
  HandlerTable* table;             ///< 0x00: Descriptor table
  std::uint8_t unknown_0x08[0x38]; ///< 0x08..0x3F: Unknown (template object fields)
};

static_assert(std::is_standard_layout_v<HandlerObject>);
static_assert(offsetof(HandlerObject, table) == 0x00);
static_assert(sizeof(HandlerObject) == 0x40);

/// @brief Engine "handler" hash-table node (0x20 bytes).
struct HandlerNode {
  HandlerNode* next;            ///< 0x00: Next node in the chain
  HandlerNode* prev;            ///< 0x08: Previous node in the chain
  std::uint32_t hash;           ///< 0x10: EffectType hash
  std::uint8_t unknown_0x14[4]; ///< 0x14: Unknown (alignment padding)
  HandlerObject* handler;       ///< 0x18: Handler object
};

static_assert(std::is_standard_layout_v<HandlerNode>);
static_assert(offsetof(HandlerNode, hash) == 0x10);
static_assert(offsetof(HandlerNode, handler) == 0x18);
static_assert(sizeof(HandlerNode) == 0x20);

/// @brief Header of the handler hash table (site of FUN_180489040).
/// @note Bucket array = buckets, length = (mask+1)*0x10. The header is exactly the per-kind entry
///       of the registry root (see HandlerRegistryRoot).
struct HandlerHashTable {  std::uint8_t unknown_0x00[0x18]; ///< 0x00..0x17: Unknown
  void* buckets;                   ///< 0x18: Bucket array
  std::uint8_t unknown_0x20[0x10]; ///< 0x20..0x2F: Unknown
  std::uint64_t mask;              ///< 0x30: Bucket-count mask
  std::uint8_t unknown_0x38[0x8];  ///< 0x38..0x3F: Unknown (alignment padding)
};

static_assert(std::is_standard_layout_v<HandlerHashTable>);
static_assert(offsetof(HandlerHashTable, buckets) == 0x18);
static_assert(offsetof(HandlerHashTable, mask) == 0x30);
static_assert(sizeof(HandlerHashTable) == 0x40);

/// @brief Element size of the hash-table bucket array (site of FUN_1806083f0: bucket array length
///   = (mask+1)*0x10).
inline constexpr std::size_t kHandlerBucketBytes = 0x10;

/// @brief Handler registry root (established by FUN_1804891b0(root)): one hash table per kind.
/// @note +0x00 effects table (kind=2), +0x40 collections table (kind=3), +0x80 requirements table.
///       The engine's runtime registry and the initialization-time registry are different objects,
///       so destruction must first verify that the root memory is readable.
struct HandlerRegistryRoot {
  HandlerHashTable effects;      ///< 0x00: Effects table (kind=2)
  HandlerHashTable collections;  ///< 0x40: Collections table (kind=3)
  HandlerHashTable requirements; ///< 0x80: Requirements table
};

static_assert(std::is_standard_layout_v<HandlerRegistryRoot>);
static_assert(offsetof(HandlerRegistryRoot, effects) == 0x00);
static_assert(offsetof(HandlerRegistryRoot, collections) == 0x40);
static_assert(offsetof(HandlerRegistryRoot, requirements) == 0x80);
static_assert(sizeof(HandlerRegistryRoot) == 0xc0);

} // namespace ykkz000::civ6
