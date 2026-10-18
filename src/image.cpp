#include "image.h"
#include "io_utils.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <webp/decode.h>
#include <webp/encode.h>

namespace {

constexpr auto VP8_TAG = std::to_array<Byte>({'V', 'P', '8', ' '});

// Owns a libwebp allocation. libwebp hands buffers back either as a return
// value or through a uint8_t** out-parameter; std::out_ptr adopts the latter.
struct WebPFreeDeleter {
  static void operator()(uint8_t *buffer) noexcept { WebPFree(buffer); }
};
using WebPBuffer = std::unique_ptr<uint8_t, WebPFreeDeleter>;

// The complete VP8 chunk (header, data and pad byte) of a WEBP file, or an
// empty span if there is none or the container is malformed. A final odd-size
// VP8 chunk missing its pad byte is not returned either: copied as-is it would
// misalign every chunk the output appends after it.
[[nodiscard]] std::span<const Byte>
findLossyVp8Chunk(std::span<const Byte> webp_data) {
  try {
    for (const RiffChunk &chunk : riffChunks(parseRiffWebP(webp_data))) {
      if (std::ranges::equal(chunk.tag, VP8_TAG) &&
          chunk.whole.size() == 8 + chunk.data.size() + (chunk.data.size() & 1U)) {
        return chunk.whole;
      }
    }
  } catch (const std::exception &) {
    // Not a container we can reuse a chunk from: the caller re-encodes.
  }
  return {};
}

// WebPGetFeatures only parses enough of the stream to report its header, so a
// truncated VP8 payload passes the header checks. Force a full decode to prove
// the bitstream is intact. MODE_YUV is the VP8 decoder's native output, so this
// skips the YUV->RGBA conversion pass and holds ~1.5 bytes/pixel instead of 4
// (~60 MB rather than ~160 MB at the pixel cap) for pixels that are discarded.
// Only valid for lossy covers, which is the sole caller.
[[nodiscard]] bool decodesFullyAsYuv(std::span<const Byte> webp_data, int width,
                                     int height) {
  int decoded_width = 0;
  int decoded_height = 0;
  uint8_t *u_plane = nullptr;
  uint8_t *v_plane = nullptr;
  int luma_stride = 0;
  int uv_stride = 0;
  // WebPDecodeYUV allocates one block and returns its luma pointer; the chroma
  // planes point into that same block, so freeing the luma pointer frees all.
  const WebPBuffer luma(WebPDecodeYUV(
      webp_data.data(), webp_data.size(), &decoded_width, &decoded_height,
      &u_plane, &v_plane, &luma_stride, &uv_stride));
  return static_cast<bool>(luma) && decoded_width == width &&
         decoded_height == height;
}

void reEncodeAsLossy(vBytes &image_vec, const uint8_t *rgb, int w, int h,
                     float quality) {
  // Local contract guard: dimensions arrive from a decoded image that already
  // passed the upstream pixel cap, so this only trips on a programming error.
  // It rejects non-positive dimensions and keeps the stride computation below
  // from overflowing int before either reaches the encoder.
  if (w <= 0 || h <= 0 || w > std::numeric_limits<int>::max() / 3) {
    throw std::runtime_error(
        "Image File Error: Unsafe WEBP dimensions for re-encoding.");
  }
  WebPBuffer output;
  const std::size_t output_size =
      WebPEncodeRGB(rgb, w, h, w * 3, quality, std::out_ptr(output));
  if (output_size == 0 || !output) {
    throw std::runtime_error(
        "Image File Error: Failed to re-encode WEBP image.");
  }
  const auto vp8 =
      findLossyVp8Chunk(std::span<const Byte>(output.get(), output_size));
  if (vp8.empty()) {
    throw std::runtime_error(
        "Image File Error: Re-encoded WEBP image has an invalid VP8 payload.");
  }
  image_vec.assign(vp8.begin(), vp8.end());
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

  if (features.format == 1 && features.has_alpha == 0) {
    // Reuse the original VP8 payload when it already matches the output format.
    // Nothing here needs pixels, only proof that the stream decodes, so
    // validate in YUV and never allocate the RGBA image at all.
    if (const auto vp8 = findLossyVp8Chunk(byteSpan(image_vec)); !vp8.empty()) {
      if (!decodesFullyAsYuv(byteSpan(image_vec), features.width,
                             features.height)) {
        throw std::runtime_error(
            "Image File Error: Failed to fully decode WEBP image.");
      }
      const auto offset = static_cast<std::size_t>(vp8.data() - image_vec.data());
      const std::size_t length = vp8.size();
      std::memmove(image_vec.data(), image_vec.data() + offset, length);
      image_vec.resize(length);
      return {features.width, features.height};
    }
  }

  // Lossless, alpha, or non-VP8 covers are re-encoded so the embedded output
  // stays a simple lossy VP8 bitstream accepted by target platforms. This path
  // needs the actual pixels, and decoding them is also the full-decode
  // integrity check for these covers. The output keeps only the VP8 chunk, so
  // any alpha would be dropped anyway: decode straight to RGB (a quarter less
  // memory than RGBA) and spare the encoder an alpha plane.
  int decoded_width = 0;
  int decoded_height = 0;
  const WebPBuffer rgb(WebPDecodeRGB(image_vec.data(), image_vec.size(),
                                     &decoded_width, &decoded_height));
  if (!rgb || decoded_width != features.width ||
      decoded_height != features.height) {
    throw std::runtime_error(
        "Image File Error: Failed to fully decode WEBP image.");
  }
  constexpr float ENCODE_QUALITY = 75.0f;
  reEncodeAsLossy(image_vec, rgb.get(), decoded_width, decoded_height,
                  ENCODE_QUALITY);
  return {features.width, features.height};
}
