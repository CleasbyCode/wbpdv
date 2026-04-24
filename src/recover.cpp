#include "recover.h"
#include "base64.h"
#include "compression.h"
#include "encryption.h"
#include "io_utils.h"
#include <algorithm>
#include <cctype>
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

struct RiffChunkView {
  std::span<const Byte> data{};
};

enum class ByteOrder : Byte { little = 0, big = 1 };

[[nodiscard]] std::optional<std::size_t> searchSig(std::span<const Byte> vec, const auto& sig) {
  if (vec.size() < sig.size()) {
    return std::nullopt;
  }
  auto it = std::search(vec.begin(), vec.end(), sig.begin(), sig.end());
  if (it == vec.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::distance(vec.begin(), it));
}

[[nodiscard]] std::uint16_t readExif16(std::span<const Byte> data, std::size_t offset, ByteOrder byte_order) {
  return byte_order == ByteOrder::big ? readBe16At(data, offset) : readLe16At(data, offset);
}

[[nodiscard]] std::uint32_t readExif32(std::span<const Byte> data, std::size_t offset, ByteOrder byte_order) {
  return byte_order == ByteOrder::big ? readBe32At(data, offset) : readLe32At(data, offset);
}

[[nodiscard]] std::span<const Byte> parseRiffWebP(std::span<const Byte> image_data) {
  constexpr const char* CORRUPT_RIFF_ERROR = "Image File Error: Invalid or corrupt WEBP container.";
  requireSpanRange(image_data, 0, 12, CORRUPT_RIFF_ERROR);
  if (!std::ranges::equal(image_data.first(RIFF_CHUNK_TAG.size()), RIFF_CHUNK_TAG) ||
      !std::ranges::equal(image_data.subspan(8, WEBP_CHUNK_TAG.size()), WEBP_CHUNK_TAG))
    throw std::runtime_error("Error: Not a valid WEBP image.");
  const std::size_t riff_total_size =
      checkedAddSize(8, static_cast<std::size_t>(readLe32At(image_data, 4)), CORRUPT_RIFF_ERROR);
  if (riff_total_size - 8 < WEBP_CHUNK_TAG.size() || !spanHasRange(image_data, 0, riff_total_size))
    throw std::runtime_error(CORRUPT_RIFF_ERROR);
  return image_data.first(riff_total_size);
}

[[nodiscard]] std::optional<RiffChunkView> findChunk(std::span<const Byte> riff_file, const auto& tag) {
  constexpr const char* CORRUPT_RIFF_ERROR = "Image File Error: Invalid or corrupt WEBP container.";
  for (std::size_t offset = 12; offset < riff_file.size();) {
    requireSpanRange(riff_file, offset, 8, CORRUPT_RIFF_ERROR);
    const std::uint32_t chunk_size = readLe32At(riff_file, offset + 4);
    const std::size_t data_offset = checkedAddSize(offset, 8, CORRUPT_RIFF_ERROR);
    const std::size_t padded_chunk_size =
        checkedAddSize(static_cast<std::size_t>(chunk_size), static_cast<std::size_t>(chunk_size & 1U), CORRUPT_RIFF_ERROR);
    if (!spanHasRange(riff_file, data_offset, padded_chunk_size))
      throw std::runtime_error(CORRUPT_RIFF_ERROR);
    if (std::ranges::equal(riff_file.subspan(offset, tag.size()), tag))
      return RiffChunkView{.data = std::span<const Byte>(riff_file.data() + data_offset, chunk_size)};
    offset = checkedAddSize(data_offset, padded_chunk_size, CORRUPT_RIFF_ERROR);
  }
  return std::nullopt;
}

[[nodiscard]] bool hasEmbeddedIccPayload(std::span<const Byte> iccp_data) {
  const auto icc_pos = searchSig(iccp_data, ICC_PROFILE_SIGNATURE);
  if (!icc_pos || *icc_pos < ICC_SIG_OFFSET_FROM_PROFILE_START)
    return false;
  const auto profile_data = iccp_data.subspan(*icc_pos - ICC_SIG_OFFSET_FROM_PROFILE_START);
  return spanHasRange(profile_data, WEBP_EXTRACT_OFFSETS.kdf_metadata, KDF_METADATA_REGION_BYTES) &&
         searchSig(profile_data, WDV_SIGNATURE).has_value();
}

[[nodiscard]] bool hasEmbeddedBlueskyPayload(std::span<const Byte> exif_data) {
  const auto wdv_pos = searchSig(exif_data, WDV_SIGNATURE);
  return wdv_pos && *wdv_pos >= WDV_TO_KDF && spanHasRange(exif_data, *wdv_pos - WDV_TO_KDF, KDF_METADATA_REGION_BYTES);
}

[[nodiscard]] fs::path safeRecoveryPath(std::string decrypted_filename) {
  constexpr std::size_t MAX_RECOVERED_FILENAME_LEN = 255;
  if (decrypted_filename.empty() || decrypted_filename.size() > MAX_RECOVERED_FILENAME_LEN) {
    throw std::runtime_error("File Recovery Error: Recovered filename is unsafe.");
  }
  fs::path parsed(std::move(decrypted_filename));
  if (parsed.has_root_path() || parsed.has_parent_path() || parsed != parsed.filename() || !hasValidFilename(parsed)) {
    throw std::runtime_error("File Recovery Error: Recovered filename is unsafe.");
  }
  fs::path candidate = parsed.filename();
  const auto path_exists = [](const fs::path& path) {
    std::error_code exists_ec;
    const bool exists = fs::exists(path, exists_ec);
    if (exists_ec) {
      throw std::runtime_error("Write File Error: Unable to check output filename.");
    }
    return exists;
  };
  if (!path_exists(candidate)) {
    return candidate;
  }
  std::string stem = candidate.stem().string();
  if (stem.empty())
    stem = "recovered";
  const std::string ext = candidate.extension().string();
  for (std::size_t i = 1; i <= 10000; ++i) {
    fs::path next(std::format("{}_{}{}", stem, i, ext));
    if (!path_exists(next))
      return next;
  }
  throw std::runtime_error("Write File Error: Unable to create a unique output filename.");
}

void writeRecoveredFile(vBytes& image_vec, std::string filename) {
  fs::path output_path = safeRecoveryPath(std::move(filename));
  auto staged_file = createUniqueFile(output_path.parent_path(),
                                      ".wbpdv_tmp_",
                                      "",
                                      1024,
                                      "Write File Error: Unable to create temp output file: ",
                                      "Write File Error: Unable to allocate temporary output filename.");
  try {
    writeAllToFd(staged_file.fd, std::span<const Byte>(image_vec.data(), image_vec.size()));
    syncFdOrThrow(staged_file.fd, "Write File Error: Failed to sync output file: ");
    closeFdOrThrow(staged_file.fd);
    commitPathAtomically(staged_file.path,
                         output_path,
                         "Write File Error: Output file already exists.",
                         "Write File Error: Failed to commit recovered file: ");
  } catch (...) {
    closeFdNoThrow(staged_file.fd);
    cleanupPathNoThrow(staged_file.path);
    throw;
  }
  std::println("\nExtracted hidden file: {} ({} bytes).\n\nComplete! Please check your file.\n", output_path.string(), image_vec.size());
}

void recoverFromIccPath(vBytes& image_vec, std::span<const Byte> iccp_data) {
  const auto icc_pos = searchSig(iccp_data, ICC_PROFILE_SIGNATURE);
  if (!icc_pos)
    throw std::runtime_error("Image File Error: Cannot locate ICC profile in image.");
  if (*icc_pos < ICC_SIG_OFFSET_FROM_PROFILE_START || !spanHasRange(iccp_data, *icc_pos - ICC_SIG_OFFSET_FROM_PROFILE_START, 1))
    throw std::runtime_error("Image File Error: Corrupt profile location.");
  image_vec.assign(iccp_data.begin() + static_cast<std::ptrdiff_t>(*icc_pos - ICC_SIG_OFFSET_FROM_PROFILE_START), iccp_data.end());
  requireSpanRange(
      image_vec, WEBP_EXTRACT_OFFSETS.kdf_metadata, KDF_METADATA_REGION_BYTES, "File Recovery Error: Embedded profile is corrupt.");
  auto result = decryptDataFile(image_vec, WEBP_EXTRACT_OFFSETS);
  if (!result)
    throw std::runtime_error("File Recovery Error: Invalid PIN or file is corrupt.");
  zlibInflate(image_vec);
  writeRecoveredFile(image_vec, std::move(*result));
}

[[nodiscard]] vBytes extractXmpOverflowData(std::span<const Byte> xmp_data) {
  constexpr const char* XMP_ERROR = "File Extraction Error: Corrupt XMP data.";
  if (xmp_data.empty()) {
    return {};
  }
  auto rdfli_pos = searchSig(xmp_data, XMP_RDFLI_SIGNATURE);
  if (!rdfli_pos) {
    return {};
  }
  const auto start_it = std::find(
      xmp_data.begin() + static_cast<std::ptrdiff_t>(*rdfli_pos + XMP_RDFLI_SIGNATURE.size()), xmp_data.end(), static_cast<Byte>('>'));
  if (start_it == xmp_data.end()) {
    throw std::runtime_error(XMP_ERROR);
  }
  std::size_t base64_begin = static_cast<std::size_t>(std::distance(xmp_data.begin(), start_it)) + 1;
  while (base64_begin < xmp_data.size() && std::isspace(static_cast<unsigned char>(xmp_data[base64_begin])) != 0) {
    ++base64_begin;
  }
  if (base64_begin >= xmp_data.size()) {
    throw std::runtime_error(XMP_ERROR);
  }
  auto end_it = std::find(xmp_data.begin() + static_cast<std::ptrdiff_t>(base64_begin), xmp_data.end(), XMP_BASE64_END_DELIM);
  if (end_it == xmp_data.end()) {
    throw std::runtime_error(XMP_ERROR);
  }
  std::size_t base64_end = static_cast<std::size_t>(std::distance(xmp_data.begin(), end_it));
  while (base64_end > base64_begin && std::isspace(static_cast<unsigned char>(xmp_data[base64_end - 1])) != 0) {
    --base64_end;
  }
  if (base64_end <= base64_begin) {
    throw std::runtime_error(XMP_ERROR);
  }
  std::span<const Byte> base64_span(xmp_data.data() + static_cast<std::ptrdiff_t>(base64_begin), base64_end - base64_begin);
  vBytes decoded;
  appendBase64AsBinary(base64_span, decoded, MAX_BLUESKY_UPLOAD_SIZE);
  return decoded;
}

void recoverFromBlueskyPath(vBytes& image_vec, std::span<const Byte> exif_data, std::span<const Byte> xmp_data) {
  constexpr const char* CORRUPT_ERROR = "File Recovery Error: Embedded EXIF data is corrupt.";
  constexpr std::size_t TIFF_HEADER_OFFSET = 6;
  constexpr std::size_t MAX_IFD_ENTRIES = 64;

  // Bluesky keeps overflow encrypted bytes in XMP when the EXIF artist field
  // is not large enough to hold the full ciphertext.
  vBytes xmp_overflow = extractXmpOverflowData(xmp_data);
  requireSpanRange(exif_data, 0, EXIF_HEADER_SIGNATURE.size(), CORRUPT_ERROR);
  if (!std::ranges::equal(exif_data.first(EXIF_HEADER_SIGNATURE.size()), EXIF_HEADER_SIGNATURE)) {
    throw std::runtime_error("Image File Error: Cannot locate EXIF data in image.");
  }
  auto wdv_pos = searchSig(exif_data, WDV_SIGNATURE);
  if (!wdv_pos.has_value()) {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  if (*wdv_pos < WDV_TO_KDF) {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  const std::size_t kdf_offset = *wdv_pos - WDV_TO_KDF;
  const std::size_t encrypted_offset = checkedAddSize(*wdv_pos, WDV_PLUS_TRAILING, CORRUPT_ERROR);
  requireSpanRange(exif_data, TIFF_HEADER_OFFSET, 8, CORRUPT_ERROR);
  ByteOrder byte_order{};
  if (exif_data[TIFF_HEADER_OFFSET] == 'M' && exif_data[TIFF_HEADER_OFFSET + 1] == 'M') {
    byte_order = ByteOrder::big;
  } else if (exif_data[TIFF_HEADER_OFFSET] == 'I' && exif_data[TIFF_HEADER_OFFSET + 1] == 'I') {
    byte_order = ByteOrder::little;
  } else {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  if (readExif16(exif_data, TIFF_HEADER_OFFSET + 2, byte_order) != 42) {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  const std::size_t ifd_count_offset =
      checkedAddSize(TIFF_HEADER_OFFSET, static_cast<std::size_t>(readExif32(exif_data, TIFF_HEADER_OFFSET + 4, byte_order)), CORRUPT_ERROR);
  requireSpanRange(exif_data, ifd_count_offset, 2, CORRUPT_ERROR);
  const std::uint16_t ifd_count = readExif16(exif_data, ifd_count_offset, byte_order);
  if (ifd_count == 0 || ifd_count > MAX_IFD_ENTRIES) {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  std::optional<std::size_t> artist_end;
  for (std::uint16_t i = 0; i < ifd_count; ++i) {
    const std::size_t entry_index_bytes = checkedMulSize(static_cast<std::size_t>(i), 12, CORRUPT_ERROR);
    const std::size_t entry_off = checkedAddSize(checkedAddSize(ifd_count_offset, 2, CORRUPT_ERROR), entry_index_bytes, CORRUPT_ERROR);
    requireSpanRange(exif_data, entry_off, 12, CORRUPT_ERROR);
    const std::uint16_t tag = readExif16(exif_data, entry_off, byte_order);
    if (tag == 0x013B) {
      const std::uint16_t type = readExif16(exif_data, entry_off + 2, byte_order);
      const std::uint32_t artist_count = readExif32(exif_data, entry_off + 4, byte_order);
      const std::uint32_t artist_tiff_offset = readExif32(exif_data, entry_off + 8, byte_order);
      if (type != 0x0002 || artist_count == 0) {
        throw std::runtime_error(CORRUPT_ERROR);
      }
      const std::size_t artist_data_offset =
          checkedAddSize(TIFF_HEADER_OFFSET, static_cast<std::size_t>(artist_tiff_offset), CORRUPT_ERROR);
      if (!spanHasRange(exif_data, artist_data_offset, static_cast<std::size_t>(artist_count))) {
        throw std::runtime_error(CORRUPT_ERROR);
      }
      artist_end = checkedAddSize(artist_data_offset, static_cast<std::size_t>(artist_count), CORRUPT_ERROR);
      break;
    }
  }
  if (!artist_end.has_value()) {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  if (*wdv_pos >= *artist_end || encrypted_offset > *artist_end) {
    throw std::runtime_error(CORRUPT_ERROR);
  }
  image_vec.assign(exif_data.begin(), exif_data.begin() + static_cast<std::ptrdiff_t>(*artist_end));
  if (!xmp_overflow.empty()) {
    if (image_vec.size() > MAX_PROGRAM_FILE_SIZE - xmp_overflow.size()) {
      throw std::runtime_error("File Recovery Error: Embedded file exceeds maximum program size.");
    }
    image_vec.insert(image_vec.end(), xmp_overflow.begin(), xmp_overflow.end());
  }
  const ProfileOffsets bluesky_offsets = {kdf_offset, encrypted_offset};
  requireSpanRange(image_vec, bluesky_offsets.kdf_metadata, KDF_METADATA_REGION_BYTES, CORRUPT_ERROR);
  auto result = decryptDataFile(image_vec, bluesky_offsets);
  if (!result) {
    throw std::runtime_error("File Recovery Error: Invalid PIN or file is corrupt.");
  }
  zlibInflate(image_vec);
  writeRecoveredFile(image_vec, std::move(*result));
}
} // namespace

void recoverData(vBytes& image_vec) {
  const auto riff_file = parseRiffWebP(std::span<const Byte>(image_vec.data(), image_vec.size()));

  if (const auto iccp_chunk = findChunk(riff_file, ICCP_CHUNK_TAG); iccp_chunk.has_value() && hasEmbeddedIccPayload(iccp_chunk->data)) {
    recoverFromIccPath(image_vec, iccp_chunk->data);
    return;
  }

  if (const auto exif_chunk = findChunk(riff_file, EXIF_CHUNK_TAG); exif_chunk.has_value() && hasEmbeddedBlueskyPayload(exif_chunk->data)) {
    const auto xmp_chunk = findChunk(riff_file, XMP_CHUNK_TAG);
    recoverFromBlueskyPath(image_vec, exif_chunk->data, xmp_chunk.has_value() ? xmp_chunk->data : std::span<const Byte>{});
    return;
  }
  throw std::runtime_error("Image File Error: Signature check failure. This is not a valid wbpdv file-embedded image.");
}
