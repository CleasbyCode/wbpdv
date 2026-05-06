#include "image.h"
#include "io_utils.h"

#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <webp/decode.h>
#include <webp/encode.h>

namespace {

constexpr std::size_t WEBP_HEADER_LENGTH = 12;
constexpr auto RIFF_TAG = std::to_array<Byte>({'R', 'I', 'F', 'F'});
constexpr auto WEBP_TAG = std::to_array<Byte>({'W', 'E', 'B', 'P'});
constexpr auto VP8_TAG = std::to_array<Byte>({'V', 'P', '8', ' '});

// Returns (offset, length) of the VP8 chunk (8-byte chunk header + padded data) within
// webp_data, or nullopt if not found. Lets callers either memmove in place or copy
// from a foreign buffer without an extra intermediate allocation.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> findLossyVp8Chunk(std::span<const Byte> webp_data) {
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
  const std::size_t riff_total_size = checkedAddSize(8, riff_payload_size, "Image File Error: Invalid or corrupt WEBP container.");
  if (!spanHasRange(webp_data, 0, riff_total_size)) {
    return std::nullopt;
  }
  for (std::size_t offset = WEBP_HEADER_LENGTH; offset < riff_total_size;) {
    if (!spanHasRange(webp_data, offset, 8)) {
      return std::nullopt;
    }
    const std::uint32_t chunk_size = readLe32At(webp_data, offset + 4);
    const std::size_t padded_chunk_size = static_cast<std::size_t>(chunk_size) + ((chunk_size & 1U) ? 1U : 0U);
    const std::size_t total_chunk_bytes = checkedAddSize(8, padded_chunk_size, "Image File Error: Invalid or corrupt WEBP container.");
    if (!spanHasRange(webp_data, offset, total_chunk_bytes)) {
      return std::nullopt;
    }
    if (std::equal(VP8_TAG.begin(), VP8_TAG.end(), webp_data.begin() + static_cast<std::ptrdiff_t>(offset))) {
      return std::make_pair(offset, total_chunk_bytes);
    }
    offset = checkedAddSize(offset, total_chunk_bytes, "Image File Error: Invalid or corrupt WEBP container.");
  }
  return std::nullopt;
}

void reEncodeAsLossy(vBytes& image_vec, float quality) {
  int w = 0, h = 0;
  uint8_t* rgba = WebPDecodeRGBA(image_vec.data(), image_vec.size(), &w, &h);
  if (!rgba)
    throw std::runtime_error("Image File Error: Failed to decode WEBP image for re-encoding.");
  if (w <= 0 || w > std::numeric_limits<int>::max() / 4) {
    WebPFree(rgba);
    throw std::runtime_error("Image File Error: Unsafe WEBP width for re-encoding.");
  }
  uint8_t* output = nullptr;
  const std::size_t output_size = WebPEncodeRGBA(rgba, w, h, w * 4, quality, &output);
  WebPFree(rgba);
  if (output_size == 0 || !output) {
    if (output)
      WebPFree(output);
    throw std::runtime_error("Image File Error: Failed to re-encode WEBP image.");
  }
  const auto loc = findLossyVp8Chunk(std::span<const Byte>(output, output_size));
  if (!loc) {
    WebPFree(output);
    throw std::runtime_error("Image File Error: Re-encoded WEBP image has an invalid VP8 payload.");
  }
  image_vec.assign(output + loc->first, output + loc->first + loc->second);
  WebPFree(output);
}
} // namespace

ImageInfo validateAndPrepareImage(vBytes& image_vec) {
  WebPBitstreamFeatures features{};
  const VP8StatusCode feature_status = WebPGetFeatures(image_vec.data(), image_vec.size(), &features);
  if (feature_status != VP8_STATUS_OK) {
    throw std::runtime_error("Error: Not a valid WEBP image.");
  }
  if (features.width <= 0 || features.height <= 0) {
    throw std::runtime_error("Image File Error: Invalid WEBP dimensions.");
  }
  const std::uint64_t pixel_count = static_cast<std::uint64_t>(features.width) * static_cast<std::uint64_t>(features.height);
  if (pixel_count > MAX_COVER_IMAGE_PIXELS) {
    throw std::runtime_error("Image File Error: Cover image dimensions exceed safe decode limits.");
  }
  if (features.has_animation != 0) {
    throw std::runtime_error("Image File Error: WEBP animation image files not supported.");
  }

  if (features.format == 1 && features.has_alpha == 0) {
    // Reuse the original VP8 payload when it already matches the output format.
    if (auto loc = findLossyVp8Chunk(std::span<const Byte>(image_vec.data(), image_vec.size()))) {
      std::memmove(image_vec.data(), image_vec.data() + loc->first, loc->second);
      image_vec.resize(loc->second);
      return {features.width, features.height};
    }
  }
  constexpr float ENCODE_QUALITY = 75.0f;
  reEncodeAsLossy(image_vec, ENCODE_QUALITY);
  return {features.width, features.height};
}
