#include "image.h"

#include <webp/decode.h>
#include <webp/encode.h>

#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace {
constexpr std::size_t WEBP_HEADER_LENGTH = 12;
constexpr auto RIFF_TAG = std::to_array<char>({'R', 'I', 'F', 'F'});
constexpr auto WEBP_TAG = std::to_array<char>({'W', 'E', 'B', 'P'});

// Re-encode a WebP image as lossy at the given quality.
// Decodes to RGBA, then encodes back. Replaces image_vec in-place.
void reEncodeAsLossy(vBytes& image_vec, float quality) {
	int decoded_width = 0, decoded_height = 0;
	uint8_t* rgba = WebPDecodeRGBA(image_vec.data(), image_vec.size(), &decoded_width, &decoded_height);
	if (!rgba) {
		throw std::runtime_error("Image File Error: Failed to decode WEBP image for re-encoding.");
	}

	if (decoded_width <= 0 || decoded_width > std::numeric_limits<int>::max() / 4) {
		WebPFree(rgba);
		throw std::runtime_error("Image File Error: Unsafe WEBP width for re-encoding.");
	}

	uint8_t* output = nullptr;
	const int rgba_stride = decoded_width * 4;
	const std::size_t output_size = WebPEncodeRGBA(
		rgba,
		decoded_width,
		decoded_height,
		rgba_stride,
		quality,
		&output);

	WebPFree(rgba);

	if (output_size == 0 || !output) {
		if (output) WebPFree(output);
		throw std::runtime_error("Image File Error: Failed to re-encode WEBP image.");
	}

	image_vec.assign(output, output + output_size);
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

	const std::uint64_t pixel_count =
		static_cast<std::uint64_t>(features.width) *
		static_cast<std::uint64_t>(features.height);
	if (pixel_count > MAX_COVER_IMAGE_PIXELS) {
		throw std::runtime_error("Image File Error: Cover image dimensions exceed safe decode limits.");
	}

	if (features.has_animation != 0) {
		throw std::runtime_error("Image File Error: WEBP animation image files not supported.");
	}

	// Re-encode as lossy WebP to minimize cover image size.
	// This also strips all existing metadata (EXIF, XMP, ICCP) and
	// converts lossless images to lossy.
	constexpr float ENCODE_QUALITY = 75.0f;
	reEncodeAsLossy(image_vec, ENCODE_QUALITY);

	// The re-encoded image is a simple-format WebP (RIFF + VP8).
	// Strip the 12-byte RIFF/WEBP header to leave just the VP8 chunk.
	if (image_vec.size() < WEBP_HEADER_LENGTH) {
		throw std::runtime_error("Error: Re-encoded WEBP image too small.");
	}
	if (!std::equal(RIFF_TAG.begin(), RIFF_TAG.end(), reinterpret_cast<const char*>(image_vec.data())) ||
		!std::equal(WEBP_TAG.begin(), WEBP_TAG.end(), reinterpret_cast<const char*>(image_vec.data()) + 8)) {
		throw std::runtime_error("Image File Error: Re-encoded WEBP image has an invalid container header.");
	}
	image_vec.erase(image_vec.begin(), image_vec.begin() + static_cast<std::ptrdiff_t>(WEBP_HEADER_LENGTH));

	return {features.width, features.height};
}
