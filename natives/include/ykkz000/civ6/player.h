#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

/// @file player.h
/// @brief Runtime layout mirror of the GameCore engine's Player-related memory
///   (GameCore_XP2_FinalRelease.dll).
/// @note This directory describes layout only: types contain only POD data members, with no virtual
///       functions, base classes, or non-trivial members. Struct names mirror the engine's nested
///       types (Player::Instance / Player::Influence, ...) so they correspond one-to-one with the
///       symbolized decompilation. Each known field pins its offset with a static_assert and gives
///       the evidence function name in the comment; because the mirror differs, only
///       "offset/semantics" are carried over here, never function RVAs. An evidence note tagged
///       "earlier symbol mirror" means the field's offset came from an older, symbol-rich build and
///       has not been individually re-verified on the release image; it is recorded for reference
///       only (not used for logic).
namespace ykkz000::civ6 {

/// @brief Engine enum GameCore::PlayerTypes (4 bytes).
/// @note This mod only treats it as a type annotation for "player type/ID" and does not depend on
///       specific enum values.
enum class PlayerTypes : std::int32_t {};

/// @brief Sentinel value the engine uses for "no/unset" player type (-1 / 0xFFFFFFFF).
/// @note The initial value of Player::Influence's m_eSuzerain (+0x418) is -1; a player-vector index
///       is not guaranteed to match the player type (city-states/barbarians skip numbers).
inline constexpr PlayerTypes kInvalidPlayerType = static_cast<PlayerTypes>(-1);

/// @brief Get the underlying integer of a PlayerTypes.
/// @param[in] type Player type.
/// @return The underlying int32 value, for range checks such as kMaxPlausiblePlayerIndex.
[[nodiscard]] inline constexpr std::int32_t PlayerTypeIndex(PlayerTypes type) noexcept {
  return static_cast<std::int32_t>(type);
}

/// @brief Engine GameCore::Player namespace (mirroring its types as nested structs).
struct Player {
  struct Instance;
  struct Influence;

  /// @brief Engine GameCore::Player::Instance.
  struct Instance {
    void** vtable;                     ///< 0x000: Object vtable
    std::uint8_t unknown_0x008[0xd0];  ///< 0x008..0x0D7: Unknown
    /// @brief 0x0D8: PlayerTypes (the player type of this Instance).
    /// @note Evidence: AI::Economic::GetOwner (*(this+0x98)+0xD8),
    ///       Player::Instance::GetLevel / IsMinor (*(this+0xD8)),
    ///       Influence::ChangeTokensReceivedModifier (*(this+0x498)+0xD8).
    PlayerTypes player_type;
    std::uint8_t unknown_0x0dc[0x5f4]; ///< 0x0DC..0x6CF: Unknown
    /// @brief 0x6D0: Cities object pointer; its +0xD0 is the head of the city chain (node next is
    ///   at +0x10).
    /// @note Evidence: Player::Stats::GetPopulation / CalculatePlayerYieldNetRate traverse it.
    void* cities;
    std::uint8_t unknown_0x6d8[8];     ///< 0x6D8..0x6DF: Unknown
    /// @brief 0x6E0: Diplomacy object pointer.
    /// @note Evidence: City::Instance::CalculateYield gets GetPlayer(owner) and passes it to a
    ///       diplomacy function via [RAX+0x6E0].
    void* diplomacy;
    std::uint8_t unknown_0x6e8[0x18];  ///< 0x6E8..0x6FF: Unknown
    /// @brief 0x700: Resources object pointer.
    /// @note Evidence (earlier symbol mirror):
    ///       Resources::GetResourceAmount(*(Resources**)(player+0x700), ...).
    void* resources;
    std::uint8_t unknown_0x708[0x18];  ///< 0x708..0x71F: Unknown
    /// @brief 0x720: Religion object pointer.
    /// @note Evidence (earlier symbol mirror):
    ///       Player::Religion::GetWorshipBuilding(*(Religion**)(player+0x720)).
    void* religion;
    std::uint8_t unknown_0x728[0x20];  ///< 0x728..0x747: Unknown
    /// @brief 0x748: Player::Influence* (a pointer, not an embedded object).
    /// @note Evidence: Exports::Player::GetInfluence: *(Influence**)(instance+0x748);
    ///       Influence::UpdateSuzerain: ReturnLeviedMilitary(*(Influence**)(player+0x748), ...).
    Influence* influence;
    std::uint8_t unknown_0x750[0x30];  ///< 0x750..0x77F: Unknown
    /// @brief 0x780: Treasury object pointer.
    /// @note Evidence (earlier symbol mirror):
    ///       Player::Treasury::GetTotalMaintenance(*(Treasury**)(player+0x780)).
    void* treasury;
  };

  /// @brief Engine GameCore::Player::Influence (city-state diplomacy/suzerain state, one per
  ///   player).
  /// @note The constructor (Influence::Influence) initializes FAutoVariables in order: the value of
  ///       "m_eSuzerain" (+0x408) is at +0x418 with initial value -1; +0x498 is the host
  ///       Player::Instance* (zeroed).
  struct Influence {
    void** vtable;                      ///< 0x000: Object vtable
    std::uint8_t unknown_0x008[0x280];  ///< 0x008..0x287: Unknown
    /// @brief 0x288: int32 count-table pointer, indexed by PlayerTypes.
    /// @note Evidence: Influence::UpdateSuzerain
    ///       `*(int*)(*(this+0x288))[playerType] += 1`. Semantics to be confirmed.
    std::int32_t* per_player_counts_0x288;
    std::uint8_t unknown_0x290[8];      ///< 0x290..0x297: Unknown
    /// @brief 0x298: Table pointer indexed by PlayerTypes; UpdateSuzerain uses it for the
    ///   open-borders check.
    /// @note Semantics to be confirmed.
    std::int32_t* open_borders_0x298;
    std::uint8_t unknown_0x2a0[0x138];  ///< 0x2A0..0x3D7: Unknown
    /// @brief 0x3D8..0x3FF: VariantMap of per-tributary yield modifiers (data + bounds).
    /// @note Evidence (earlier symbol mirror): Player::Influence::Get/SetYieldModifierPerTributary
    ///       uses +0x3D8/+0x3E8/+0x3F0. Semantics to be confirmed; recorded as an opaque range.
    std::uint8_t per_tributary_modifier_0x3d8[0x28];
    std::uint8_t unknown_0x400[0x18];   ///< 0x400..0x417: Unknown
    /// @brief 0x418: PlayerTypes -- the suzerain of the city-state this Influence belongs to
    ///   (m_eSuzerain).
    /// @note Evidence: Influence::GetSuzerain simply returns *(PlayerTypes*)(this+0x418);
    ///       UpdateSuzerain writes this value back when the suzerain changes.
    PlayerTypes suzerain;
    std::uint8_t unknown_0x41c[0x7c];   ///< 0x41C..0x497: Unknown
    /// @brief 0x498: Player::Instance* -- the host player of this Influence.
    /// @note Evidence: Influence::ChangeTokensReceivedModifier dispatches it as a signal argument
    ///       and reads the host player type via *(this+0x498)+0xD8.
    Instance* owner;
  };

  /// @brief Engine GameCore::Player::Manager (returned by Player::Manager::EditInstance()).
  struct Manager {
    void** vtable;                      ///< 0x00: Object vtable
    std::uint8_t unknown_0x008[0x18];   ///< 0x08..0x1F: Unknown
    /// @brief 0x20: Player::Instance*[], indexed by PlayerTypes with stride 8; valid indices are
    ///   < 0x40.
    /// @note Evidence: Influence::ChangeTokensReceivedModifier
    ///       (*(*(manager+0x20) + playerType*8)).
    Instance** players;
    std::uint8_t unknown_0x028[0x2b20]; ///< 0x28..0x2B47: Unknown
    /// @brief 0x2B48: int32 table pointer, indexed by PlayerTypes (-1 == invalid).
    /// @note Evidence: Influence::UpdateSuzerain (*(manager+0x2b48) + playerType*4, checking
    ///       0x40 < playerType before indexing out of range). Semantics to be confirmed.
    std::int32_t* player_index_table_0x2b48;
    std::uint8_t unknown_0x2b50[8];     ///< 0x2B50..0x2B57: Tail padding
  };

  /// @brief Engine GameCore::Player::Stats.
  struct Stats {
    void** vtable;                      ///< 0x000: Object vtable
    std::uint8_t unknown_0x008[0xe10];  ///< 0x008..0xE17: Unknown
    /// @brief 0xE18: Indirection pointer to the root of the city chain.
    /// @note Player::Stats::GetPopulation walks the city chain along [this+0xE18] -> +0x6D0
    ///       (Cities) -> +0xD0 (chain head) (node next at +0x10) and accumulates city+0x268
    ///       (population).
    void* city_list_root;
  };

  /// @brief Engine Player::CityID (8 bytes): {int16 player; int16 component tag (always 2); int32
  ///   serial number}.
  /// @note Evidence: the write sequence of Lua::Utility::GetCityID / Exports::District::GetCityID
  ///       (*(short*)out = owner; *(short*)(out+2) = 2; *(int*)(out+4) = serial).
  struct CityID {
    std::int16_t player_type;  ///< 0x00: Owning player (short)
    std::int16_t kind;         ///< 0x02: Component type tag (2 for a city)
    std::int32_t id;           ///< 0x04: City serial number (i.e. City::Instance+0xA8)
  };
};

static_assert(std::is_standard_layout_v<Player::Instance>);
static_assert(offsetof(Player::Instance, player_type) == 0xd8);
static_assert(offsetof(Player::Instance, cities) == 0x6d0);
static_assert(offsetof(Player::Instance, diplomacy) == 0x6e0);
static_assert(offsetof(Player::Instance, resources) == 0x700);
static_assert(offsetof(Player::Instance, religion) == 0x720);
static_assert(offsetof(Player::Instance, influence) == 0x748);
static_assert(offsetof(Player::Instance, treasury) == 0x780);

static_assert(std::is_standard_layout_v<Player::Influence>);
static_assert(offsetof(Player::Influence, per_player_counts_0x288) == 0x288);
static_assert(offsetof(Player::Influence, open_borders_0x298) == 0x298);
static_assert(offsetof(Player::Influence, per_tributary_modifier_0x3d8) == 0x3d8);
static_assert(offsetof(Player::Influence, suzerain) == 0x418);
static_assert(offsetof(Player::Influence, owner) == 0x498);

static_assert(std::is_standard_layout_v<Player::Manager>);
static_assert(offsetof(Player::Manager, players) == 0x20);
static_assert(offsetof(Player::Manager, player_index_table_0x2b48) == 0x2b48);

static_assert(std::is_standard_layout_v<Player::Stats>);
static_assert(offsetof(Player::Stats, city_list_root) == 0xe18);

static_assert(std::is_standard_layout_v<Player::CityID>);
static_assert(sizeof(Player::CityID) == 8);
static_assert(offsetof(Player::CityID, kind) == 0x02);
static_assert(offsetof(Player::CityID, id) == 0x04);

} // namespace ykkz000::civ6
