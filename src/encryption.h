#pragma once

#include "common.h"
#include "profile_template.h"

#include <cstddef>
#include <optional>

// ---------------------------------------------------------------------------
// Embed / extract layout. Offsets are absolute into the corresponding profile
// template (standard ICCP or Bluesky EXIF). static_asserts below keep the
// binary templates and the named constants locked together.
// ---------------------------------------------------------------------------

struct ProfileOffsets {
  std::size_t kdf_metadata;
  std::size_t encrypted_file;
};

// --- Shared RIFF / VP8X prefix --------------------------------------------
// RIFF(12) + "VP8X"(4) + size(4) + flags(1) + reserved(3) + canvas(6) = 0x1E
inline constexpr std::size_t VP8X_CHUNK_END = 0x1E;
inline constexpr std::size_t VP8X_FLAGS_OFFSET = 0x14;
inline constexpr std::size_t VP8X_CANVAS_WIDTH_MINUS_ONE_OFFSET = 0x18;  // le24
inline constexpr std::size_t VP8X_CANVAS_HEIGHT_MINUS_ONE_OFFSET = 0x1B; // le24

// libwebp VP8X feature flag bits
inline constexpr Byte VP8X_FLAG_XMP = 0x04;
inline constexpr Byte VP8X_FLAG_EXIF = 0x08;
inline constexpr Byte VP8X_FLAG_ICCP = 0x20;
inline constexpr Byte VP8X_FLAGS_EXIF_AND_XMP =
    static_cast<Byte>(VP8X_FLAG_EXIF | VP8X_FLAG_XMP); // 0x0C

// Bytes from start of WDV signature through its trailing marker; ciphertext
// is appended immediately after this span.
inline constexpr std::size_t WDV_SIGNATURE_SPAN = 8;

// --- Standard (ICCP) layout -----------------------------------------------
// Template size == start of encrypted region (ciphertext is appended).
inline constexpr ProfileOffsets WEBP_EMBED_OFFSETS = {0x270, 0x37C};
// recover strips the leading RIFF+VP8X+ICCP-chunk-header so offsets shrink.
inline constexpr std::size_t CONTAINER_HEADER_STRIP = 0x2A;
inline constexpr ProfileOffsets WEBP_EXTRACT_OFFSETS = {
    WEBP_EMBED_OFFSETS.kdf_metadata - CONTAINER_HEADER_STRIP,
    WEBP_EMBED_OFFSETS.encrypted_file - CONTAINER_HEADER_STRIP};

inline constexpr std::size_t ICCP_RIFF_CHUNK_SIZE_OFFSET = 0x22; // le32
inline constexpr std::size_t ICCP_PROFILE_SIZE_OFFSET = 0x26;    // be32
// Absolute file offset of first ICCP payload byte (after "ICCP" + size).
inline constexpr std::size_t ICCP_PAYLOAD_FILE_OFFSET = 0x26;
// recover treats the profile base as CONTAINER_HEADER_STRIP (skips the 4-byte
// ICC size field at the start of the ICCP payload) so extract offsets line up.

// --- Bluesky (EXIF + optional XMP overflow) layout ------------------------
inline constexpr ProfileOffsets BLUESKY_EMBED_OFFSETS = {0xC0, 0x104};
inline constexpr std::size_t BLUESKY_EXIF_CHUNK_SIZE_OFFSET = 0x22; // le32
inline constexpr std::size_t BLUESKY_ARTIST_COUNT_OFFSET = 0x6A;    // be32
inline constexpr std::size_t BLUESKY_ARTIST_DATA_START = 0xB0;
// Fixed prefix before variable EXIF artist payload (for chunk size calc).
inline constexpr std::size_t BLUESKY_EXIF_FIXED_PREFIX = 0x26;
// Distance from WDV signature back to the KDF metadata region in EXIF artist.
inline constexpr std::size_t BLUESKY_WDV_TO_KDF = 60;

inline constexpr std::size_t KDF_METADATA_REGION_BYTES = 56;

inline constexpr auto WDV_SIGNATURE =
    std::to_array<Byte>({0xB4, 0x77, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9});
inline constexpr auto ICC_PROFILE_SIGNATURE =
    std::to_array<Byte>({0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42});
inline constexpr std::size_t ICC_SIG_OFFSET_FROM_PROFILE_START = 8;
inline constexpr auto EXIF_HEADER_SIGNATURE =
    std::to_array<Byte>({0x45, 0x78, 0x69, 0x66, 0x00, 0x00});
inline constexpr auto XMP_CHUNK_TAG =
    std::to_array<Byte>({0x58, 0x4D, 0x50, 0x20});
inline constexpr auto XMP_RDFLI_SIGNATURE =
    std::to_array<Byte>({0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69});
inline constexpr Byte XMP_BASE64_END_DELIM = 0x3C;
inline constexpr std::size_t MAX_EXIF_ENCRYPTED_DATA = 65000;

inline constexpr auto ICCP_CHUNK_TAG =
    std::to_array<Byte>({0x49, 0x43, 0x43, 0x50});

// --- Template / offset lockstep -------------------------------------------
namespace embed_layout_detail {

consteval bool bytesEqualAt(const auto &haystack, std::size_t offset,
                            const auto &needle) {
  if (offset + needle.size() > haystack.size()) {
    return false;
  }
  for (std::size_t i = 0; i < needle.size(); ++i) {
    if (haystack[offset + i] != needle[i]) {
      return false;
    }
  }
  return true;
}

consteval std::size_t findBytes(const auto &haystack, const auto &needle) {
  if (needle.empty() || haystack.size() < needle.size()) {
    return haystack.size();
  }
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    if (bytesEqualAt(haystack, i, needle)) {
      return i;
    }
  }
  return haystack.size();
}

} // namespace embed_layout_detail

static_assert(WEBP_PROFILE_TEMPLATE.size() ==
              WEBP_EMBED_OFFSETS.encrypted_file);
static_assert(WEBP_BLUESKY_EXIF_TEMPLATE.size() ==
              BLUESKY_EMBED_OFFSETS.encrypted_file);

static_assert(embed_layout_detail::findBytes(WEBP_PROFILE_TEMPLATE,
                                             WDV_SIGNATURE) +
                  WDV_SIGNATURE_SPAN ==
              WEBP_EMBED_OFFSETS.encrypted_file);
static_assert(embed_layout_detail::findBytes(WEBP_BLUESKY_EXIF_TEMPLATE,
                                             WDV_SIGNATURE) +
                  WDV_SIGNATURE_SPAN ==
              BLUESKY_EMBED_OFFSETS.encrypted_file);

static_assert(embed_layout_detail::findBytes(WEBP_BLUESKY_EXIF_TEMPLATE,
                                             WDV_SIGNATURE) -
                  BLUESKY_WDV_TO_KDF ==
              BLUESKY_EMBED_OFFSETS.kdf_metadata);

// "mntrRGB" is 8 bytes into the extract-base profile (which itself starts
// CONTAINER_HEADER_STRIP into the full template).
static_assert(embed_layout_detail::findBytes(WEBP_PROFILE_TEMPLATE,
                                             ICC_PROFILE_SIGNATURE) ==
              CONTAINER_HEADER_STRIP + ICC_SIG_OFFSET_FROM_PROFILE_START);

static_assert(WEBP_PROFILE_TEMPLATE[VP8X_FLAGS_OFFSET] == VP8X_FLAG_ICCP);
static_assert(WEBP_BLUESKY_EXIF_TEMPLATE[VP8X_FLAGS_OFFSET] == VP8X_FLAG_EXIF);
static_assert(BLUESKY_ARTIST_DATA_START < BLUESKY_EMBED_OFFSETS.kdf_metadata);
static_assert(WEBP_EXTRACT_OFFSETS.kdf_metadata + CONTAINER_HEADER_STRIP ==
              WEBP_EMBED_OFFSETS.kdf_metadata);
static_assert(WEBP_EXTRACT_OFFSETS.encrypted_file + CONTAINER_HEADER_STRIP ==
              WEBP_EMBED_OFFSETS.encrypted_file);

[[nodiscard]] std::uint64_t encryptDataFile(vBytes &profile_vec,
                                            vBytes &data_vec,
                                            const std::string &data_filename,
                                            const ProfileOffsets &offsets);

// Interactive PIN entry then decrypt. Prefer decryptDataFileWithPin in tests.
[[nodiscard]] std::optional<std::string>
decryptDataFile(vBytes &profile_vec, const ProfileOffsets &offsets);

// Decrypt using an explicit recovery PIN (no terminal I/O).
[[nodiscard]] std::optional<std::string>
decryptDataFileWithPin(vBytes &profile_vec, const ProfileOffsets &offsets,
                       std::uint64_t recovery_pin);
