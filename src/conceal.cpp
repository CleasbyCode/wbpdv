#include "conceal.h"
#include "base64.h"
#include "compression.h"
#include "encryption.h"
#include "image.h"
#include "io_utils.h"
#include "profile_template.h"

#include <limits>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t FILENAME_MAX_LEN = 20;
constexpr std::size_t BLUESKY_MIN_METADATA_BYTES =
    WEBP_BLUESKY_EXIF_TEMPLATE.size() + 64;
constexpr std::size_t MASTODON_UPLOAD_SIZE = 16ULL * 1024 * 1024;
constexpr std::size_t TUMBLR_UPLOAD_SIZE = 9ULL * 1024 * 1024;
constexpr std::string_view OUTPUT_OVERFLOW_ERROR =
    "File Size Error: Output file size overflow.";

} // namespace

std::size_t checkedStandardOutputSize(std::size_t profile_size,
                                      std::size_t iccp_data_size,
                                      std::size_t image_size) {
  std::size_t output_size = checkedAddSize(
      profile_size, iccp_data_size & 1U, OUTPUT_OVERFLOW_ERROR);
  output_size =
      checkedAddSize(output_size, image_size, OUTPUT_OVERFLOW_ERROR);
  if (output_size > MAX_PROGRAM_FILE_SIZE) {
    throw std::runtime_error(
        "File Size Error: Final output exceeds maximum size limit of 1GB.");
  }
  return output_size;
}

namespace {

void appendSv(vBytes &out, std::string_view text) {
  appendBytes(out,
              std::span<const Byte>(reinterpret_cast<const Byte *>(text.data()),
                                    text.size()),
              OUTPUT_OVERFLOW_ERROR);
}

void appendRiffPadding(vBytes &out, std::size_t payload_size) {
  if ((payload_size & 1U) != 0) {
    out.push_back(0x00);
  }
}

[[nodiscard]] std::uint32_t checkedU32(std::size_t value,
                                       std::string_view error) {
  if (value >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error(std::string(error));
  }
  return static_cast<std::uint32_t>(value);
}

void setProfileDimensions(vBytes &profile_vec, int width, int height) {
  writeLe24At(profile_vec, VP8X_CANVAS_WIDTH_MINUS_ONE_OFFSET,
              static_cast<std::uint32_t>(width - 1));
  writeLe24At(profile_vec, VP8X_CANVAS_HEIGHT_MINUS_ONE_OFFSET,
              static_cast<std::uint32_t>(height - 1));
}

[[nodiscard]] vBytes buildXmpChunk(std::span<const Byte> overflow_data) {
  constexpr std::string_view XMP_HEADER =
      "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Go XMP SDK 1.0\">"
      "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
      "rdf:about=\"\">"
      "<dc:creator><rdf:Seq><rdf:li>";
  constexpr std::string_view XMP_FOOTER =
      "</rdf:li></rdf:Seq></dc:creator></rdf:Description></rdf:RDF></"
      "x:xmpmeta>\n"
      "<?xpacket end=\"w\"?>";

  vBytes chunk(8);
  chunk[0] = 'X';
  chunk[1] = 'M';
  chunk[2] = 'P';
  chunk[3] = ' ';

  // Bluesky spills encrypted bytes into XMP once the EXIF artist field is full.
  const std::size_t base64_size = checkedMulSize(
      (checkedAddSize(
           overflow_data.size(), 2,
           "File Size Error: XMP chunk payload exceeds size limits.") /
       3),
      4, "File Size Error: XMP chunk payload exceeds size limits.");
  std::size_t reserve_size =
      checkedAddSize(8, XMP_HEADER.size(),
                     "File Size Error: XMP chunk payload exceeds size limits.");
  reserve_size =
      checkedAddSize(reserve_size, base64_size,
                     "File Size Error: XMP chunk payload exceeds size limits.");
  reserve_size =
      checkedAddSize(reserve_size, XMP_FOOTER.size(),
                     "File Size Error: XMP chunk payload exceeds size limits.");
  chunk.reserve(reserve_size);
  appendSv(chunk, XMP_HEADER);
  binaryToBase64(overflow_data, chunk);
  appendSv(chunk, XMP_FOOTER);
  writeLe32At(
      chunk, 4,
      checkedU32(chunk.size() - 8,
                 "File Size Error: XMP chunk payload exceeds 4GB limit."));
  return chunk;
}

void finalizeRiffSize(vBytes &data) {
  if (data.size() < 8 ||
      data.size() - 8 > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(
        "File Size Error: Output file too large for WEBP format.");
  }
  writeLe32At(data, 0x04, static_cast<std::uint32_t>(data.size() - 8));
}

void writeOutputFile(std::span<const Byte> data, std::uint64_t pin,
                     bool is_bluesky) {
  auto output_file = createUniqueFile(
      {}, ".wbpdv_tmp_", "", 2048,
      "Write Error: Unable to create staged output file: ",
      "Write Error: Unable to allocate staged output filename.");

  writeAllToFd(output_file.fd(), data);
  syncFdOrThrow(output_file.fd(), "Write Error: Failed to sync output file: ");
  output_file.closeOrThrow();
  const fs::path saved_path = commitToUniquePathAtomically(
      output_file.path(), {}, "wbpdv_", ".webp", 2048,
      "Write Error: Failed to commit output file: ",
      "Write Error: Unable to allocate output filename.");
  // The staged path was renamed into place. Drop ownership so the destructor
  // does not attempt to unlink the old staging name.
  output_file.release();

  std::println("\nPlatform compatibility for output image:-\n");

  if (is_bluesky) {
    std::println(" ✓ Bluesky. (Only share this \"file-embedded\" WEBP image on "
                 "Bluesky).");
  } else {
    if (data.size() <= MASTODON_UPLOAD_SIZE) {
      std::println(" ✓ Mastodon");
    }
    if (data.size() <= TUMBLR_UPLOAD_SIZE) {
      std::println(" ✓ Tumblr");
    }
  }

  std::println("\nSaved \"file-embedded\" WEBP image: {} ({} bytes).",
               saved_path.string(), data.size());
  std::println("\nRecovery PIN: [***{}***]\n\nImportant: Keep your PIN safe, "
               "so that you can extract the hidden file.\n\nComplete!\n",
               pin);
}

void validateDataFilename(const fs::path &data_file_path,
                          const std::string &data_filename) {
  if (!hasSafeEmbeddedFilename(data_file_path.filename())) {
    throw std::runtime_error(
        "Data File Error: Embedded filename is unsafe. "
        "Filenames may not begin with '.' or '-'.");
  }
  if (data_filename.size() > FILENAME_MAX_LEN) {
    throw std::runtime_error(
        "Data File Error: For compatibility requirements, length of data "
        "filename must not exceed 20 characters.");
  }
}

void validateConcealSizes(const vBytes &image_vec, std::size_t data_file_size,
                          bool is_bluesky) {
  if (is_bluesky &&
      checkedAddSize(image_vec.size(), BLUESKY_MIN_METADATA_BYTES,
                     "File Size Error: Output file size overflow.") >
          MAX_BLUESKY_UPLOAD_SIZE) {
    throw std::runtime_error(
        "File Size Error: Cover image is too large for Bluesky output.");
  }
  const std::size_t combined_size =
      checkedAddSize(image_vec.size(), data_file_size,
                     "File Size Error: Combined size overflow.");
  if (!is_bluesky && combined_size > MAX_PROGRAM_FILE_SIZE) {
    throw std::runtime_error("File Size Error: Combined size of image and data "
                             "file exceeds maximum size limit of 1GB.");
  }
}

void concealBluesky(vBytes &image_vec, vBytes &data_vec,
                    const std::string &data_filename, int width, int height) {
  vBytes profile_vec(WEBP_BLUESKY_EXIF_TEMPLATE.begin(),
                     WEBP_BLUESKY_EXIF_TEMPLATE.end());
  setProfileDimensions(profile_vec, width, height);
  const std::uint64_t pin = encryptDataFile(
      profile_vec, data_vec, data_filename, BLUESKY_EMBED_OFFSETS);

  const std::size_t encrypted_size =
      profile_vec.size() - BLUESKY_EMBED_OFFSETS.encrypted_file;
  vBytes xmp_chunk;
  if (encrypted_size > MAX_EXIF_ENCRYPTED_DATA) {
    const std::size_t exif_end =
        BLUESKY_EMBED_OFFSETS.encrypted_file + MAX_EXIF_ENCRYPTED_DATA;
    xmp_chunk = buildXmpChunk(byteSpan(profile_vec).subspan(exif_end));
    profile_vec.resize(exif_end);
    profile_vec[VP8X_FLAGS_OFFSET] = VP8X_FLAGS_EXIF_AND_XMP;
  }

  const std::size_t exif_data_size =
      profile_vec.size() - BLUESKY_EXIF_FIXED_PREFIX;
  writeLe32At(
      profile_vec, BLUESKY_EXIF_CHUNK_SIZE_OFFSET,
      checkedU32(exif_data_size,
                 "File Size Error: EXIF chunk data exceeds 4GB limit."));
  writeBe32At(
      profile_vec, BLUESKY_ARTIST_COUNT_OFFSET,
      checkedU32(profile_vec.size() - BLUESKY_ARTIST_DATA_START,
                 "File Size Error: Artist IFD count exceeds 4GB limit."));

  vBytes output_vec;
  std::size_t output_reserve =
      checkedAddSize(VP8X_CHUNK_END, image_vec.size(),
                     "File Size Error: Output file size overflow.");
  output_reserve =
      checkedAddSize(output_reserve, profile_vec.size() - VP8X_CHUNK_END,
                     "File Size Error: Output file size overflow.");
  output_reserve =
      checkedAddSize(output_reserve, xmp_chunk.size(),
                     "File Size Error: Output file size overflow.");
  output_reserve = checkedAddSize(
      output_reserve, 2, "File Size Error: Output file size overflow.");
  output_vec.reserve(output_reserve);
  appendBytes(output_vec,
              std::span<const Byte>(profile_vec.data(), VP8X_CHUNK_END),
              OUTPUT_OVERFLOW_ERROR);
  appendBytes(output_vec, byteSpan(image_vec), OUTPUT_OVERFLOW_ERROR);
  appendBytes(output_vec, byteSpan(profile_vec).subspan(VP8X_CHUNK_END),
              OUTPUT_OVERFLOW_ERROR);
  appendRiffPadding(output_vec, exif_data_size);
  if (!xmp_chunk.empty()) {
    appendBytes(output_vec, byteSpan(xmp_chunk), OUTPUT_OVERFLOW_ERROR);
    appendRiffPadding(output_vec, xmp_chunk.size() - 8);
  }

  finalizeRiffSize(output_vec);
  if (output_vec.size() > MAX_BLUESKY_UPLOAD_SIZE) {
    throw std::runtime_error("File Size Error: Output file (" +
                             std::to_string(output_vec.size()) +
                             " bytes) exceeds the Bluesky upload limit of " +
                             std::to_string(MAX_BLUESKY_UPLOAD_SIZE) +
                             " bytes.\n"
                             "Try a smaller cover image or data file.");
  }
  writeOutputFile(byteSpan(output_vec), pin, true);
}

void concealStandard(vBytes &image_vec, vBytes &data_vec,
                     const std::string &data_filename, int width, int height) {
  vBytes profile_vec(WEBP_PROFILE_TEMPLATE.begin(),
                     WEBP_PROFILE_TEMPLATE.end());
  setProfileDimensions(profile_vec, width, height);
  const std::uint64_t pin =
      encryptDataFile(profile_vec, data_vec, data_filename, WEBP_EMBED_OFFSETS);

  const std::size_t iccp_data_size =
      profile_vec.size() - ICCP_PAYLOAD_FILE_OFFSET;
  const std::uint32_t iccp_data_size_u32 = checkedU32(
      iccp_data_size, "File Size Error: ICCP chunk data exceeds 4GB limit.");
  writeLe32At(profile_vec, ICCP_RIFF_CHUNK_SIZE_OFFSET, iccp_data_size_u32);
  writeBe32At(profile_vec, ICCP_PROFILE_SIZE_OFFSET, iccp_data_size_u32);
  const std::size_t final_output_size = checkedStandardOutputSize(
      profile_vec.size(), iccp_data_size, image_vec.size());
  profile_vec.reserve(final_output_size);
  appendRiffPadding(profile_vec, iccp_data_size);
  appendBytes(profile_vec, byteSpan(image_vec), OUTPUT_OVERFLOW_ERROR);
  finalizeRiffSize(profile_vec);
  if (profile_vec.size() != final_output_size) {
    throw std::runtime_error("Internal Error: Standard output size mismatch.");
  }
  writeOutputFile(byteSpan(profile_vec), pin, false);
}

} // namespace

void concealData(vBytes &image_vec, Option option,
                 const fs::path &data_file_path) {
  const bool is_bluesky = (option == Option::Bluesky);
  const std::string data_filename = data_file_path.filename().string();
  validateDataFilename(data_file_path, data_filename);
  const auto [width, height] = validateAndPrepareImage(image_vec);

  vBytes data_vec = readFile(data_file_path);
  // Validate the bytes from the opened file, not an earlier path-based stat;
  // the path may have been replaced between independent observations.
  validateConcealSizes(image_vec, data_vec.size(), is_bluesky);
  zlibDeflate(data_vec,
              is_bluesky ? MAX_BLUESKY_UPLOAD_SIZE : MAX_PROGRAM_FILE_SIZE);

  if (is_bluesky) {
    concealBluesky(image_vec, data_vec, data_filename, width, height);
    return;
  }

  concealStandard(image_vec, data_vec, data_filename, width, height);
}
