#include "recover.h"
#include "base64.h"
#include "compression.h"
#include "encryption.h"
#include "io_utils.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
namespace {

constexpr auto RIFF_CHUNK_TAG = std::to_array<Byte>({'R', 'I', 'F', 'F'});
constexpr auto WEBP_CHUNK_TAG = std::to_array<Byte>({'W', 'E', 'B', 'P'});
constexpr auto EXIF_CHUNK_TAG = std::to_array<Byte>({'E', 'X', 'I', 'F'});
constexpr std::size_t WDV_TO_KDF = 60;
constexpr std::size_t WDV_PLUS_TRAILING = 8;

struct RiffChunkSet {
  std::optional<std::span<const Byte>> iccp;
  std::optional<std::span<const Byte>> exif;
  std::optional<std::span<const Byte>> xmp;
};

enum class ByteOrder : Byte { little = 0, big = 1 };

constexpr std::size_t TIFF_HEADER_OFFSET = 6;
constexpr std::size_t MAX_IFD_ENTRIES = 64;
constexpr std::uint16_t TIFF_MAGIC = 42;
constexpr std::uint16_t EXIF_ARTIST_TAG = 0x013B;
constexpr std::uint16_t EXIF_ASCII_TYPE = 0x0002;

template <std::size_t N>
[[nodiscard]] std::optional<std::size_t>
searchSig(std::span<const Byte> vec, const std::array<Byte, N> &sig) {
  if constexpr (N == 0) {
    return 0;
  }
  if (vec.size() < N) {
    return std::nullopt;
  }
  const Byte *const begin = vec.data();
  const Byte *const end = begin + (vec.size() - N + 1);
  const Byte *current = begin;
  while (current < end) {
    const auto remaining = static_cast<std::size_t>(end - current);
    const void *found = std::memchr(current, sig[0], remaining);
    if (found == nullptr) {
      return std::nullopt;
    }
    current = static_cast<const Byte *>(found);
    if (N == 1 || std::memcmp(current, sig.data(), N) == 0) {
      return static_cast<std::size_t>(current - begin);
    }
    ++current;
  }
  return std::nullopt;
}

[[nodiscard]] std::uint16_t readExif16(std::span<const Byte> data,
                                       std::size_t offset,
                                       ByteOrder byte_order) {
  return byte_order == ByteOrder::big ? readBe16At(data, offset)
                                      : readLe16At(data, offset);
}

[[nodiscard]] std::uint32_t readExif32(std::span<const Byte> data,
                                       std::size_t offset,
                                       ByteOrder byte_order) {
  return byte_order == ByteOrder::big ? readBe32At(data, offset)
                                      : readLe32At(data, offset);
}

[[nodiscard]] std::span<const Byte>
parseRiffWebP(std::span<const Byte> image_data) {
  constexpr const char *CORRUPT_RIFF_ERROR =
      "Image File Error: Invalid or corrupt WEBP container.";
  requireSpanRange(image_data, 0, 12, CORRUPT_RIFF_ERROR);
  if (!std::ranges::equal(image_data.first(RIFF_CHUNK_TAG.size()),
                          RIFF_CHUNK_TAG) ||
      !std::ranges::equal(image_data.subspan(8, WEBP_CHUNK_TAG.size()),
                          WEBP_CHUNK_TAG)) {
    throw std::runtime_error("Error: Not a valid WEBP image.");
  }
  const std::size_t riff_total_size =
      checkedAddSize(8, static_cast<std::size_t>(readLe32At(image_data, 4)),
                     CORRUPT_RIFF_ERROR);
  if (riff_total_size - 8 < WEBP_CHUNK_TAG.size() ||
      !spanHasRange(image_data, 0, riff_total_size)) {
    throw std::runtime_error(CORRUPT_RIFF_ERROR);
  }
  return image_data.first(riff_total_size);
}

[[nodiscard]] RiffChunkSet findRelevantChunks(std::span<const Byte> riff_file) {
  constexpr const char *CORRUPT_RIFF_ERROR =
      "Image File Error: Invalid or corrupt WEBP container.";
  RiffChunkSet chunks;
  for (std::size_t offset = 12; offset < riff_file.size();) {
    requireSpanRange(riff_file, offset, 8, CORRUPT_RIFF_ERROR);
    const std::uint32_t chunk_size = readLe32At(riff_file, offset + 4);
    const std::size_t data_offset =
        checkedAddSize(offset, 8, CORRUPT_RIFF_ERROR);
    const std::size_t padded_chunk_size = checkedAddSize(
        static_cast<std::size_t>(chunk_size),
        static_cast<std::size_t>(chunk_size & 1U), CORRUPT_RIFF_ERROR);
    if (!spanHasRange(riff_file, data_offset, padded_chunk_size)) {
      throw std::runtime_error(CORRUPT_RIFF_ERROR);
    }
    const auto tag = riff_file.subspan(offset, ICCP_CHUNK_TAG.size());
    const auto chunk_data = std::span<const Byte>(
        riff_file.data() + static_cast<std::ptrdiff_t>(data_offset),
        chunk_size);
    if (!chunks.iccp && std::ranges::equal(tag, ICCP_CHUNK_TAG)) {
      chunks.iccp = chunk_data;
    } else if (!chunks.exif && std::ranges::equal(tag, EXIF_CHUNK_TAG)) {
      chunks.exif = chunk_data;
    } else if (!chunks.xmp && std::ranges::equal(tag, XMP_CHUNK_TAG)) {
      chunks.xmp = chunk_data;
    }
    offset = checkedAddSize(data_offset, padded_chunk_size, CORRUPT_RIFF_ERROR);
  }
  return chunks;
}

[[nodiscard]] bool hasEmbeddedIccPayload(std::span<const Byte> iccp_data) {
  const auto icc_pos = searchSig(iccp_data, ICC_PROFILE_SIGNATURE);
  if (!icc_pos || *icc_pos < ICC_SIG_OFFSET_FROM_PROFILE_START) {
    return false;
  }
  const auto profile_data =
      iccp_data.subspan(*icc_pos - ICC_SIG_OFFSET_FROM_PROFILE_START);
  return spanHasRange(profile_data, WEBP_EXTRACT_OFFSETS.kdf_metadata,
                      KDF_METADATA_REGION_BYTES) &&
         searchSig(profile_data, WDV_SIGNATURE).has_value();
}

[[nodiscard]] bool hasEmbeddedBlueskyPayload(std::span<const Byte> exif_data) {
  const auto wdv_pos = searchSig(exif_data, WDV_SIGNATURE);
  return wdv_pos && *wdv_pos >= WDV_TO_KDF &&
         spanHasRange(exif_data, *wdv_pos - WDV_TO_KDF,
                      KDF_METADATA_REGION_BYTES);
}

[[nodiscard]] ByteOrder parseExifByteOrder(std::span<const Byte> exif_data) {
  constexpr const char *CORRUPT_ERROR =
      "File Recovery Error: Embedded EXIF data is corrupt.";
  requireSpanRange(exif_data, TIFF_HEADER_OFFSET, 8, CORRUPT_ERROR);
  if (exif_data[TIFF_HEADER_OFFSET] == 'M' &&
      exif_data[TIFF_HEADER_OFFSET + 1] == 'M') {
    return ByteOrder::big;
  }
  if (exif_data[TIFF_HEADER_OFFSET] == 'I' &&
      exif_data[TIFF_HEADER_OFFSET + 1] == 'I') {
    return ByteOrder::little;
  }
  throw std::runtime_error(CORRUPT_ERROR);
}

[[nodiscard]] std::size_t findExifArtistEnd(std::span<const Byte> exif_data) {
  constexpr const char *CORRUPT_ERROR =
      "File Recovery Error: Embedded EXIF data is corrupt.";
  requireSpanRange(exif_data, 0, EXIF_HEADER_SIGNATURE.size(), CORRUPT_ERROR);
  if (!std::ranges::equal(exif_data.first(EXIF_HEADER_SIGNATURE.size()),
                          EXIF_HEADER_SIGNATURE)) {
    throw std::runtime_error(
        "Image File Error: Cannot locate EXIF data in image.");
  }

  const ByteOrder byte_order = parseExifByteOrder(exif_data);
  if (readExif16(exif_data, TIFF_HEADER_OFFSET + 2, byte_order) != TIFF_MAGIC) {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  const std::size_t ifd_count_offset =
      checkedAddSize(TIFF_HEADER_OFFSET,
                     static_cast<std::size_t>(readExif32(
                         exif_data, TIFF_HEADER_OFFSET + 4, byte_order)),
                     CORRUPT_ERROR);
  requireSpanRange(exif_data, ifd_count_offset, 2, CORRUPT_ERROR);
  const std::uint16_t ifd_count =
      readExif16(exif_data, ifd_count_offset, byte_order);
  if (ifd_count == 0 || ifd_count > MAX_IFD_ENTRIES) {
    throw std::runtime_error(CORRUPT_ERROR);
  }

  for (std::uint16_t i = 0; i < ifd_count; ++i) {
    const std::size_t entry_index_bytes =
        checkedMulSize(static_cast<std::size_t>(i), 12, CORRUPT_ERROR);
    const std::size_t entry_off =
        checkedAddSize(checkedAddSize(ifd_count_offset, 2, CORRUPT_ERROR),
                       entry_index_bytes, CORRUPT_ERROR);
    requireSpanRange(exif_data, entry_off, 12, CORRUPT_ERROR);
    if (readExif16(exif_data, entry_off, byte_order) != EXIF_ARTIST_TAG) {
      continue;
    }
    const std::uint16_t type = readExif16(exif_data, entry_off + 2, byte_order);
    const std::uint32_t artist_count =
        readExif32(exif_data, entry_off + 4, byte_order);
    const std::uint32_t artist_tiff_offset =
        readExif32(exif_data, entry_off + 8, byte_order);
    if (type != EXIF_ASCII_TYPE || artist_count == 0) {
      throw std::runtime_error(CORRUPT_ERROR);
    }
    const std::size_t artist_data_offset = checkedAddSize(
        TIFF_HEADER_OFFSET, static_cast<std::size_t>(artist_tiff_offset),
        CORRUPT_ERROR);
    requireSpanRange(exif_data, artist_data_offset,
                     static_cast<std::size_t>(artist_count), CORRUPT_ERROR);
    return checkedAddSize(artist_data_offset,
                          static_cast<std::size_t>(artist_count),
                          CORRUPT_ERROR);
  }
  throw std::runtime_error(CORRUPT_ERROR);
}

[[nodiscard]] bool pathExistsOrThrow(const fs::path &path) {
  std::error_code exists_ec;
  const bool exists = fs::exists(path, exists_ec);
  if (exists_ec) {
    throw std::runtime_error(
        "Write File Error: Unable to check output filename.");
  }
  return exists;
}

[[nodiscard]] fs::path uniqueRecoveryPath(const fs::path &candidate) {
  if (!pathExistsOrThrow(candidate)) {
    return candidate;
  }

  std::string stem = candidate.stem().string();
  if (stem.empty()) {
    stem = "recovered";
  }

  const std::string ext = candidate.extension().string();
  for (std::size_t i = 1; i <= 10000; ++i) {
    fs::path next(std::format("{}_{}{}", stem, i, ext));
    if (!pathExistsOrThrow(next)) {
      return next;
    }
  }

  throw std::runtime_error(
      "Write File Error: Unable to create a unique output filename.");
}

[[nodiscard]] fs::path safeRecoveryPath(std::string decrypted_filename) {
  constexpr std::size_t MAX_RECOVERED_FILENAME_LEN = 255;
  if (decrypted_filename.empty() ||
      decrypted_filename.size() > MAX_RECOVERED_FILENAME_LEN) {
    throw std::runtime_error(
        "File Recovery Error: Recovered filename is unsafe.");
  }
  fs::path parsed(std::move(decrypted_filename));
  if (parsed.has_root_path() || parsed.has_parent_path() ||
      parsed != parsed.filename() || !hasValidFilename(parsed)) {
    throw std::runtime_error(
        "File Recovery Error: Recovered filename is unsafe.");
  }
  return uniqueRecoveryPath(parsed.filename());
}

void writeRecoveredFile(vBytes &image_vec, std::string filename) {
  fs::path output_path = safeRecoveryPath(std::move(filename));
  auto staged_file = createUniqueFile(
      output_path.parent_path(), ".wbpdv_tmp_", "", 1024,
      "Write File Error: Unable to create temp output file: ",
      "Write File Error: Unable to allocate temporary output filename.");
  try {
    writeAllToFd(staged_file.fd, byteSpan(image_vec));
    syncFdOrThrow(staged_file.fd,
                  "Write File Error: Failed to sync output file: ");
    closeFdOrThrow(staged_file.fd);
    commitPathAtomically(staged_file.path, output_path,
                         "Write File Error: Output file already exists.",
                         "Write File Error: Failed to commit recovered file: ");
  } catch (...) {
    closeFdNoThrow(staged_file.fd);
    cleanupPathNoThrow(staged_file.path);
    throw;
  }
  std::println("\nExtracted hidden file: {} ({} bytes).\n\nComplete! Please "
               "check your file.\n",
               output_path.string(), image_vec.size());
}

void decryptInflateAndWrite(vBytes &image_vec, const ProfileOffsets &offsets,
                            std::string_view corrupt_error) {
  requireSpanRange(image_vec, offsets.kdf_metadata, KDF_METADATA_REGION_BYTES,
                   corrupt_error);
  auto result = decryptDataFile(image_vec, offsets);
  if (!result) {
    throw std::runtime_error(
        "File Recovery Error: Invalid PIN or file is corrupt.");
  }

  zlibInflate(image_vec);
  writeRecoveredFile(image_vec, std::move(*result));
}

void recoverFromIccPath(vBytes &image_vec, std::span<const Byte> iccp_data) {
  const auto icc_pos = searchSig(iccp_data, ICC_PROFILE_SIGNATURE);
  if (!icc_pos) {
    throw std::runtime_error(
        "Image File Error: Cannot locate ICC profile in image.");
  }
  if (*icc_pos < ICC_SIG_OFFSET_FROM_PROFILE_START) {
    throw std::runtime_error("Image File Error: Corrupt profile location.");
  }

  // iccp_data is a span into image_vec; shift the profile to the front in place
  // and truncate, instead of allocating a separate profile vector.
  const std::size_t iccp_offset_in_image =
      static_cast<std::size_t>(iccp_data.data() - image_vec.data());
  const std::size_t offset_within_iccp =
      *icc_pos - ICC_SIG_OFFSET_FROM_PROFILE_START;
  const std::size_t profile_offset = iccp_offset_in_image + offset_within_iccp;
  const std::size_t profile_length = iccp_data.size() - offset_within_iccp;
  std::memmove(image_vec.data(), image_vec.data() + profile_offset,
               profile_length);
  image_vec.resize(profile_length);

  decryptInflateAndWrite(image_vec, WEBP_EXTRACT_OFFSETS,
                         "File Recovery Error: Embedded profile is corrupt.");
}

[[nodiscard]] vBytes extractXmpOverflowData(std::span<const Byte> xmp_data) {
  constexpr const char *XMP_ERROR = "File Extraction Error: Corrupt XMP data.";
  if (xmp_data.empty()) {
    return {};
  }
  auto rdfli_pos = searchSig(xmp_data, XMP_RDFLI_SIGNATURE);
  if (!rdfli_pos) {
    return {};
  }
  const auto start_it =
      std::find(xmp_data.begin() + static_cast<std::ptrdiff_t>(
                                       *rdfli_pos + XMP_RDFLI_SIGNATURE.size()),
                xmp_data.end(), static_cast<Byte>('>'));
  if (start_it == xmp_data.end()) {
    throw std::runtime_error(XMP_ERROR);
  }
  std::size_t base64_begin =
      static_cast<std::size_t>(std::distance(xmp_data.begin(), start_it)) + 1;
  while (base64_begin < xmp_data.size() &&
         std::isspace(static_cast<unsigned char>(xmp_data[base64_begin])) !=
             0) {
    ++base64_begin;
  }
  if (base64_begin >= xmp_data.size()) {
    throw std::runtime_error(XMP_ERROR);
  }
  auto end_it =
      std::find(xmp_data.begin() + static_cast<std::ptrdiff_t>(base64_begin),
                xmp_data.end(), XMP_BASE64_END_DELIM);
  if (end_it == xmp_data.end()) {
    throw std::runtime_error(XMP_ERROR);
  }
  std::size_t base64_end =
      static_cast<std::size_t>(std::distance(xmp_data.begin(), end_it));
  while (base64_end > base64_begin && std::isspace(static_cast<unsigned char>(
                                          xmp_data[base64_end - 1])) != 0) {
    --base64_end;
  }
  if (base64_end <= base64_begin) {
    throw std::runtime_error(XMP_ERROR);
  }
  std::span<const Byte> base64_span(
      xmp_data.data() + static_cast<std::ptrdiff_t>(base64_begin),
      base64_end - base64_begin);
  vBytes decoded;
  appendBase64AsBinary(base64_span, decoded, MAX_BLUESKY_UPLOAD_SIZE);
  return decoded;
}

void recoverFromBlueskyPath(vBytes &image_vec, std::span<const Byte> exif_data,
                            std::span<const Byte> xmp_data) {
  constexpr const char *CORRUPT_ERROR =
      "File Recovery Error: Embedded EXIF data is corrupt.";

  auto wdv_pos = searchSig(exif_data, WDV_SIGNATURE);
  if (!wdv_pos.has_value()) {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  if (*wdv_pos < WDV_TO_KDF) {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  const std::size_t kdf_offset = *wdv_pos - WDV_TO_KDF;
  const std::size_t encrypted_offset =
      checkedAddSize(*wdv_pos, WDV_PLUS_TRAILING, CORRUPT_ERROR);
  const std::size_t artist_end = findExifArtistEnd(exif_data);
  if (*wdv_pos >= artist_end || encrypted_offset > artist_end) {
    throw std::runtime_error(CORRUPT_ERROR);
  }

  // Bluesky keeps overflow encrypted bytes in XMP when the EXIF artist field
  // is not large enough to hold the full ciphertext.
  vBytes xmp_overflow = extractXmpOverflowData(xmp_data);
  const std::size_t profile_size =
      checkedAddSize(artist_end, xmp_overflow.size(),
                     "File Recovery Error: Embedded file exceeds maximum "
                     "program size.");
  if (profile_size > MAX_PROGRAM_FILE_SIZE) {
    throw std::runtime_error(
        "File Recovery Error: Embedded file exceeds maximum program size.");
  }
  vBytes profile(profile_size);
  std::memcpy(profile.data(), exif_data.data(), artist_end);
  if (!xmp_overflow.empty()) {
    std::memcpy(profile.data() + static_cast<std::ptrdiff_t>(artist_end),
                xmp_overflow.data(), xmp_overflow.size());
  }
  image_vec = std::move(profile);
  const ProfileOffsets bluesky_offsets = {kdf_offset, encrypted_offset};
  decryptInflateAndWrite(image_vec, bluesky_offsets, CORRUPT_ERROR);
}
} // namespace

void recoverData(vBytes &image_vec) {
  const auto riff_file = parseRiffWebP(byteSpan(image_vec));
  const RiffChunkSet chunks = findRelevantChunks(riff_file);

  if (chunks.iccp.has_value() && hasEmbeddedIccPayload(*chunks.iccp)) {
    recoverFromIccPath(image_vec, *chunks.iccp);
    return;
  }

  if (chunks.exif.has_value() && hasEmbeddedBlueskyPayload(*chunks.exif)) {
    recoverFromBlueskyPath(image_vec, *chunks.exif,
                           chunks.xmp.value_or(std::span<const Byte>{}));
    return;
  }
  throw std::runtime_error("Image File Error: Signature check failure. This is "
                           "not a valid wbpdv file-embedded image.");
}
