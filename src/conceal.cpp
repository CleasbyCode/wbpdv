#include "conceal.h"
#include "base64.h"
#include "compression.h"
#include "encryption.h"
#include "image.h"
#include "io_utils.h"
#include "profile_template.h"

#include <format>
#include <limits>
#include <print>
#include <span>
#include <stdexcept>

namespace {

constexpr std::size_t MAX_COMBINED_SIZE = 1ULL * 1024 * 1024 * 1024, FILENAME_MAX_LEN = 20, MAX_BLUESKY_UPLOAD_SIZE = 1000000, VP8X_END = 0x1E,
                      ARTIST_DATA_START = 0xB0;

void appendSv(vBytes &out, std::string_view text) {
  out.insert(out.end(), reinterpret_cast<const Byte *>(text.data()), reinterpret_cast<const Byte *>(text.data() + text.size()));
}

[[nodiscard]] std::uint32_t checkedU32(std::size_t value, std::string_view error) {
  if (value > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) { throw std::runtime_error(std::string(error)); }
  return static_cast<std::uint32_t>(value);
}

void setProfileDimensions(vBytes &profile_vec, int width, int height) {
  writeLe24At(profile_vec, 0x18, static_cast<std::uint32_t>(width - 1));
  writeLe24At(profile_vec, 0x1B, static_cast<std::uint32_t>(height - 1));
}

[[nodiscard]] vBytes buildXmpChunk(std::span<const Byte> overflow_data) {
  constexpr std::string_view XMP_HEADER = "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                                          "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Go XMP SDK 1.0\">"
                                          "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                                          "<rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\" rdf:about=\"\">"
                                          "<dc:creator><rdf:Seq><rdf:li>";
  constexpr std::string_view XMP_FOOTER = "</rdf:li></rdf:Seq></dc:creator></rdf:Description></rdf:RDF></x:xmpmeta>\n"
                                          "<?xpacket end=\"w\"?>";

  vBytes chunk(8);
  chunk[0] = 'X';
  chunk[1] = 'M';
  chunk[2] = 'P';
  chunk[3] = ' ';
  chunk.reserve(8 + XMP_HEADER.size() + ((overflow_data.size() + 2) / 3) * 4 + XMP_FOOTER.size());
  appendSv(chunk, XMP_HEADER);
  binaryToBase64(overflow_data, chunk);
  appendSv(chunk, XMP_FOOTER);
  writeLe32At(chunk, 4, checkedU32(chunk.size() - 8, "File Size Error: XMP chunk payload exceeds 4GB limit."));
  return chunk;
}

void finalizeRiffSize(vBytes &data) {
  if (data.size() < 8 || data.size() - 8 > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("File Size Error: Output file too large for WEBP format.");
  }
  writeLe32At(data, 0x04, static_cast<std::uint32_t>(data.size() - 8));
}

void writeOutputFile(std::span<const Byte> data, std::uint64_t pin, bool is_bluesky) {
  auto output_file =
      createUniqueFile({}, "wbpdv_", ".webp", 2048, "Write Error: Unable to create output file: ", "Write Error: Unable to allocate output filename.");

  try {
    writeAllToFd(output_file.fd, data);
    syncFdOrThrow(output_file.fd, "Write Error: Failed to sync output file: ");
    closeFdOrThrow(output_file.fd);
  } catch (...) {
    closeFdNoThrow(output_file.fd);
    cleanupPathNoThrow(output_file.path);
    throw;
  }
  std::println("\nPlatform compatibility for output image:-\n");

  if (is_bluesky) {
    std::println(" ✓ Bluesky. (Only share this \"file-embedded\" WEBP image on Bluesky).");
  } else if (data.size() > 9 * 1024 * 1024) {
    std::println(" ✓ Mastodon");
  } else {
    std::println(" ✓ Mastodon");
    std::println(" ✓ Tumblr");
  }

  std::println("\nSaved \"file-embedded\" WEBP image: {} ({} bytes).", output_file.path.string(), data.size());
  std::println("\nRecovery PIN: [***{}***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n\nComplete!\n", pin);
}

} // namespace

void concealData(vBytes &image_vec, Option option, const fs::path &data_file_path) {
  const bool is_bluesky = (option == Option::Bluesky);
  const std::size_t data_file_size = getFileSizeChecked(data_file_path);
  const std::string data_filename = data_file_path.filename().string();
  if (data_filename.size() > FILENAME_MAX_LEN) throw std::runtime_error("Data File Error: For compatibility requirements, length of data filename must not exceed 20 characters.");
  const auto [width, height] = validateAndPrepareImage(image_vec);
  if (data_file_size > std::numeric_limits<std::size_t>::max() - image_vec.size()) throw std::runtime_error("File Size Error: Combined size overflow.");
  if (!is_bluesky && data_file_size + image_vec.size() > MAX_COMBINED_SIZE) throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum size limit of 1GB.");
  vBytes data_vec = readFile(data_file_path); zlibDeflate(data_vec);

  if (is_bluesky) {
    vBytes profile_vec(WEBP_BLUESKY_EXIF_TEMPLATE.begin(), WEBP_BLUESKY_EXIF_TEMPLATE.end());
    setProfileDimensions(profile_vec, width, height);
    const std::uint64_t pin = encryptDataFile(profile_vec, data_vec, data_filename, BLUESKY_EMBED_OFFSETS);

    const std::size_t encrypted_size = profile_vec.size() - BLUESKY_EMBED_OFFSETS.encrypted_file;
    vBytes xmp_chunk;
    if (encrypted_size > MAX_EXIF_ENCRYPTED_DATA) {
      const std::size_t exif_end = BLUESKY_EMBED_OFFSETS.encrypted_file + MAX_EXIF_ENCRYPTED_DATA;
      xmp_chunk = buildXmpChunk(std::span<const Byte>(profile_vec.data() + static_cast<std::ptrdiff_t>(exif_end), profile_vec.size() - exif_end));
      profile_vec.resize(exif_end);
      profile_vec[0x14] = 0x0C;
    }

    const std::size_t exif_data_size = profile_vec.size() - 0x26;
    writeLe32At(profile_vec, BLUESKY_EXIF_CHUNK_SIZE_OFFSET, checkedU32(exif_data_size, "File Size Error: EXIF chunk data exceeds 4GB limit."));
    writeBe32At(profile_vec, BLUESKY_ARTIST_COUNT_OFFSET,
                checkedU32(profile_vec.size() - ARTIST_DATA_START, "File Size Error: Artist IFD count exceeds 4GB limit."));

    vBytes output_vec;
    output_vec.reserve(VP8X_END + image_vec.size() + (profile_vec.size() - VP8X_END) + xmp_chunk.size() + 2);
    output_vec.insert(output_vec.end(), profile_vec.begin(), profile_vec.begin() + VP8X_END);
    output_vec.insert(output_vec.end(), image_vec.begin(), image_vec.end());
    output_vec.insert(output_vec.end(), profile_vec.begin() + VP8X_END, profile_vec.end());
    if ((exif_data_size & 1U) != 0) { output_vec.push_back(0x00); }
    if (!xmp_chunk.empty()) {
      output_vec.insert(output_vec.end(), xmp_chunk.begin(), xmp_chunk.end());
      if (((xmp_chunk.size() - 8) & 1U) != 0) { output_vec.push_back(0x00); }
    }

    finalizeRiffSize(output_vec);
    if (output_vec.size() > MAX_BLUESKY_UPLOAD_SIZE) {
      throw std::runtime_error("File Size Error: Output file (" + std::to_string(output_vec.size()) + " bytes) exceeds the Bluesky upload limit of " +
                               std::to_string(MAX_BLUESKY_UPLOAD_SIZE) +
                               " bytes.\n"
                               "Try a smaller cover image or data file.");
    }
    writeOutputFile(std::span<const Byte>(output_vec.data(), output_vec.size()), pin, true);
    return;
  }

  vBytes profile_vec(WEBP_PROFILE_TEMPLATE.begin(), WEBP_PROFILE_TEMPLATE.end());
  setProfileDimensions(profile_vec, width, height);
  const std::uint64_t pin = encryptDataFile(profile_vec, data_vec, data_filename, WEBP_EMBED_OFFSETS);

  const std::size_t iccp_data_size = profile_vec.size() - 0x26;
  writeLe32At(profile_vec, 0x22, checkedU32(iccp_data_size, "File Size Error: ICCP chunk data exceeds 4GB limit."));
  writeBe32At(profile_vec, 0x26, static_cast<std::uint32_t>(iccp_data_size));
  profile_vec.reserve(profile_vec.size() + image_vec.size() + 1);
  if ((iccp_data_size & 1U) != 0) { profile_vec.push_back(0x00); }
  profile_vec.insert(profile_vec.end(), image_vec.begin(), image_vec.end());
  finalizeRiffSize(profile_vec);
  writeOutputFile(std::span<const Byte>(profile_vec.data(), profile_vec.size()), pin, false);
}