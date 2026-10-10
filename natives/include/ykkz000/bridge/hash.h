#pragma once

#include <cstdint>
#include <string_view>

/// @file hash.h
/// @brief String hash implementation matching the engine (CRC-32).
/// @note The algorithm must be bit-for-bit identical to the engine's
///       GameCore::Utilities::MakeHash, or cross-boundary comparisons will mismatch.
namespace ykkz000::bridge {

/// @brief Single-byte CRC-32 iteration.
/// @param[in] crc Current CRC value.
/// @param[in] byte Byte to fold in.
/// @return The updated CRC value.
[[nodiscard]] constexpr std::uint32_t crc32_byte(std::uint32_t crc, std::uint8_t byte) noexcept {
  crc ^= byte;
  for (int bit = 0; bit < 8; ++bit) {
    crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

// Matches the engine's GameCore::Utilities::MakeHash:
// CRC-32, polynomial 0xEDB88320, initial value 0xFFFFFFFF, no final inversion.
/// @brief Compute the CRC-32 of a string.
/// @param[in] text Input text.
/// @return A 32-bit hash matching the engine's MakeHash.
[[nodiscard]] constexpr std::uint32_t crc32(std::string_view text) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char ch : text) {
    crc = crc32_byte(crc, static_cast<std::uint8_t>(ch));
  }
  return crc;
}

/// @brief Compute a C-string hash (the plugin-side implementation of Host::makeHash).
/// @param[in] text Input text; may be nullptr.
/// @return 0 when text is null, otherwise crc32(text).
[[nodiscard]] inline std::uint32_t makeHash(const char* text) noexcept {
  return text ? crc32(std::string_view{text}) : 0u;
}

} // namespace ykkz000::bridge
