#include "image.h"
#include "io_utils.h"

#include <cstring>
#include <limits>
#include <optional>
#include <print>
#include <stdexcept>
#include <utility>
#include <webp/decode.h>
#include <webp/encode.h>

namespace {

constexpr std::size_t WEBP_HEADER_LENGTH = 12;
constexpr auto RIFF_TAG = std::to_array<Byte>({'R', 'I', 'F', 'F'});
constexpr auto WEBP_TAG = std::to_array<Byte>({'W', 'E', 'B', 'P'});
constexpr auto VP8_TAG = std::to_array<Byte>({'V', 'P', '8', ' '});

struct WebPBuffer {
  uint8_t *data{};

  explicit WebPBuffer(uint8_t *buffer = nullptr) noexcept : data(buffer) {}

  ~WebPBuffer() {
    if (data != nullptr) {
      WebPFree(data);
    }
  }

  WebPBuffer(const WebPBuffer &) = delete;
  WebPBuffer &operator=(const WebPBuffer &) = delete;

  [[nodiscard]] uint8_t **outParam() noexcept { return &data; }

  [[nodiscard]] explicit operator bool() const noexcept {
    return data != nullptr;
  }
};

// Returns (offset, length) of the VP8 chunk (8-byte chunk header + padded data)
// within webp_data, or nullopt if not found. Lets callers either memmove in
// place or copy from a foreign buffer without an extra intermediate allocation.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
findLossyVp8Chunk(std::span<const Byte> webp_data) {
  if (!spanHasRange(webp_data, 0, WEBP_HEADER_LENGTH)) {
    return std::nullopt;
  }
  if (!std::equal(RIFF_TAG.begin(), RIFF_TAG.end(), webp_data.begin()) ||
      !std::equal(WEBP_TAG.begin(), WEBP_TAG.end(), webp_data.begin() + 8)) {
    return std::nullopt;
  }
  const std::size_t riff_payload_size = readLe32At(webp_data, 4);
  if (riff_payload_size < WEBP_TAG.size()) {
    return std::nullopt;
  }
  const std::size_t riff_total_size =
      checkedAddSize(8, riff_payload_size,
                     "Image File Error: Invalid or corrupt WEBP container.");
  if (!spanHasRange(webp_data, 0, riff_total_size)) {
    return std::nullopt;
  }
  for (std::size_t offset = WEBP_HEADER_LENGTH; offset < riff_total_size;) {
    if (!spanHasRange(webp_data, offset, 8)) {
      return std::nullopt;
    }
    const std::uint32_t chunk_size = readLe32At(webp_data, offset + 4);
    const std::size_t padded_chunk_size =
        static_cast<std::size_t>(chunk_size) + ((chunk_size & 1U) ? 1U : 0U);
    const std::size_t total_chunk_bytes =
        checkedAddSize(8, padded_chunk_size,
                       "Image File Error: Invalid or corrupt WEBP container.");
    if (offset > riff_total_size ||
        total_chunk_bytes > riff_total_size - offset ||
        !spanHasRange(webp_data, offset, total_chunk_bytes)) {
      return std::nullopt;
    }
    if (std::equal(VP8_TAG.begin(), VP8_TAG.end(),
                   webp_data.begin() + static_cast<std::ptrdiff_t>(offset))) {
      return std::make_pair(offset, total_chunk_bytes);
    }
    offset =
        checkedAddSize(offset, total_chunk_bytes,
                       "Image File Error: Invalid or corrupt WEBP container.");
  }
  return std::nullopt;
}

void reEncodeAsLossy(vBytes &image_vec, const uint8_t *rgba, int w, int h,
                     float quality) {
  if (w <= 0 || w > std::numeric_limits<int>::max() / 4) {
    throw std::runtime_error(
        "Image File Error: Unsafe WEBP width for re-encoding.");
  }

  WebPBuffer output;
  const std::size_t output_size =
      WebPEncodeRGBA(rgba, w, h, w * 4, quality, output.outParam());
  if (output_size == 0 || !output) {
    throw std::runtime_error(
        "Image File Error: Failed to re-encode WEBP image.");
  }
  const auto loc =
      findLossyVp8Chunk(std::span<const Byte>(output.data, output_size));
  if (!loc) {
    throw std::runtime_error(
        "Image File Error: Re-encoded WEBP image has an invalid VP8 payload.");
  }
  image_vec.resize(loc->second);
  std::memcpy(image_vec.data(), output.data + loc->first, loc->second);
}
} // namespace

ImageInfo validateAndPrepareImage(vBytes &image_vec) {
  WebPBitstreamFeatures features{};
  const VP8StatusCode feature_status =
      WebPGetFeatures(image_vec.data(), image_vec.size(), &features);
  if (feature_status != VP8_STATUS_OK) {
    throw std::runtime_error("Error: Not a valid WEBP image.");
  }
  if (features.width <= 0 || features.height <= 0) {
    throw std::runtime_error("Image File Error: Invalid WEBP dimensions.");
  }
  const std::uint64_t pixel_count = static_cast<std::uint64_t>(features.width) *
                                    static_cast<std::uint64_t>(features.height);
  if (pixel_count > MAX_COVER_IMAGE_PIXELS) {
    throw std::runtime_error(
        "Image File Error: Cover image dimensions exceed safe decode limits.");
  }
  if (features.has_animation != 0) {
    throw std::runtime_error(
        "Image File Error: WEBP animation image files not supported.");
  }

  // WebPGetFeatures only parses enough of the stream to report its header. A
  // truncated VP8 stream can therefore pass the checks above. Fully decode
  // once before either preserving the opaque VP8 payload or re-encoding it.
  // The same decoded buffer is reused by the re-encode path.
  int decoded_width = 0;
  int decoded_height = 0;
  WebPBuffer rgba(WebPDecodeRGBA(image_vec.data(), image_vec.size(),
                                 &decoded_width, &decoded_height));
  if (!rgba || decoded_width != features.width ||
      decoded_height != features.height) {
    throw std::runtime_error(
        "Image File Error: Failed to fully decode WEBP image.");
  }

  if (features.format == 1 && features.has_alpha == 0) {
    // Reuse the original VP8 payload when it already matches the output format.
    if (auto loc = findLossyVp8Chunk(byteSpan(image_vec))) {
      std::memmove(image_vec.data(), image_vec.data() + loc->first,
                   loc->second);
      image_vec.resize(loc->second);
      return {features.width, features.height};
    }
  }
  // Lossless, alpha, or non-VP8 covers are re-encoded so the embedded output
  // stays a simple lossy VP8 bitstream accepted by target platforms.
  constexpr float ENCODE_QUALITY = 75.0f;
  std::println("Cover image re-encoded to lossy WEBP (quality {:.0f}) for "
               "platform compatibility.",
               ENCODE_QUALITY);
  reEncodeAsLossy(image_vec, rgba.data, decoded_width, decoded_height,
                  ENCODE_QUALITY);
  return {features.width, features.height};
}
