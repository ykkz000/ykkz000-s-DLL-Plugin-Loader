#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/common.h>
#include <ykkz000/civ6/player.h>

/// @file game_manager.h
/// @brief Layout mirror of the engine's GameManager object.
/// @note Describes layout only; offsets pinned by static_assert.
namespace ykkz000::civ6 {

/// @brief GameManager object (returned by FUN_180044d60()).
/// @note Starting at +0x50 is the player pointer vector, indexed to get a Player::Instance*. The
///       game returns the same object from Context::Globals::EditPlayerManager() (UpdateSuzerain
///       also reads the player vector at +0x50/+0x58). A player-vector index is not guaranteed to
///       equal the player type, so when an exact match is needed, compare
///       Player::Instance::player_type item by item.
struct GameManager {
  void** vtable;                                ///< 0x00: Object vtable
  std::uint8_t unknown_0x08[0x48];              ///< 0x08..0x4F: Unknown
  VectorView<Player::Instance*> players;        ///< 0x50: Player pointer vector (begin/end/capacity)
  std::uint8_t unknown_0x68[0x1068];            ///< 0x68..0x10CF: Unknown
  VectorView<void*> unknown_vector_0x10d0;      ///< 0x10D0: Another vector (8-byte elements, unverified)
};

static_assert(std::is_standard_layout_v<GameManager>);
static_assert(offsetof(GameManager, players) == 0x50);
static_assert(offsetof(GameManager, players) +
                  offsetof(VectorView<Player::Instance*>, begin) == 0x50);
static_assert(offsetof(GameManager, players) +
                  offsetof(VectorView<Player::Instance*>, end) == 0x58);
static_assert(offsetof(GameManager, unknown_vector_0x10d0) == 0x10d0);

} // namespace ykkz000::civ6
