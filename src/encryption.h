#pragma once
#include "common.h"
#include <cstddef>
#include <optional>
struct ProfileOffsets {
  std::size_t kdf_metadata;
  std::size_t encrypted_file;
};
inline constexpr ProfileOffsets WEBP_EMBED_OFFSETS = {0x270, 0x37C};
inline constexpr std::size_t CONTAINER_HEADER_STRIP = 0x2A;
inline constexpr ProfileOffsets WEBP_EXTRACT_OFFSETS = {0x270 - CONTAINER_HEADER_STRIP, 0x37C - CONTAINER_HEADER_STRIP};
inline constexpr std::size_t KDF_METADATA_REGION_BYTES = 56, KDF_MAGIC_OFFSET = 0, KDF_ALG_OFFSET = 4, KDF_SENTINEL_OFFSET = 5, KDF_SALT_OFFSET = 8,
                             KDF_NONCE_OFFSET = 24;
inline constexpr Byte KDF_ALG_ARGON2ID13 = 1, KDF_SENTINEL = 0xA5;
enum class KdfMetadataVersion : Byte { none = 0, v2_secretbox = 2 };
inline constexpr ProfileOffsets BLUESKY_EMBED_OFFSETS = {0xC0, 0x104};
inline constexpr std::size_t BLUESKY_HEADER_STRIP = 0x26;
inline constexpr ProfileOffsets BLUESKY_EXTRACT_OFFSETS = {0xC0 - BLUESKY_HEADER_STRIP, 0x104 - BLUESKY_HEADER_STRIP};
inline constexpr std::size_t BLUESKY_EXIF_CHUNK_SIZE_OFFSET = 0x22;
inline constexpr std::size_t BLUESKY_ARTIST_COUNT_OFFSET = 0x6A;
inline constexpr std::size_t BLUESKY_ARTIST_DATA_TIFF_OFFSET = 0x84;
inline constexpr auto WDV_SIGNATURE = std::to_array<Byte>({0xB4, 0x77, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9});
inline constexpr auto ICC_PROFILE_SIGNATURE = std::to_array<Byte>({0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42});
inline constexpr std::size_t ICC_SIG_OFFSET_FROM_PROFILE_START = 8;
inline constexpr auto EXIF_HEADER_SIGNATURE = std::to_array<Byte>({0x45, 0x78, 0x69, 0x66, 0x00, 0x00});
inline constexpr auto XMP_CHUNK_TAG = std::to_array<Byte>({0x58, 0x4D, 0x50, 0x20});
inline constexpr auto XMP_RDFLI_SIGNATURE = std::to_array<Byte>({0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69});
inline constexpr Byte XMP_BASE64_END_DELIM = 0x3C;
inline constexpr std::size_t MAX_EXIF_ENCRYPTED_DATA = 65000;
inline constexpr auto ICCP_CHUNK_TAG = std::to_array<Byte>({0x49, 0x43, 0x43, 0x50});
inline constexpr std::size_t BLUESKY_ICCP_OVERHEAD = 132;
inline constexpr std::size_t MAX_ICCP_RAW_DATA = 65000;
[[nodiscard]] std::uint64_t getPin();
[[nodiscard]] std::uint64_t encryptDataFile(vBytes &profile_vec, vBytes &data_vec, const std::string &data_filename, const ProfileOffsets &offsets);
[[nodiscard]] std::optional<std::string> decryptDataFile(vBytes &profile_vec, const ProfileOffsets &offsets);
