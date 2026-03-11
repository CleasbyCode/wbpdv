#include "image.h"

#include <webp/decode.h>
#include <webp/encode.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <stdexcept>

namespace {

[[nodiscard]] std::optional<std::size_t> searchSig(const vBytes& vec, std::size_t start, const auto& sig) {
	if (vec.size() < sig.size() || start > vec.size() - sig.size()) {
		return std::nullopt;
	}
	auto it = std::search(vec.begin() + static_cast<std::ptrdiff_t>(start), vec.end(), sig.begin(), sig.end());
	if (it == vec.end()) {
		return std::nullopt;
	}
	return static_cast<std::size_t>(std::distance(vec.begin(), it));
}

// Re-encode a WebP image as lossy at the given quality.
// Decodes to RGBA, then encodes back. Replaces image_vec in-place.
void reEncodeAsLossy(vBytes& image_vec, float quality) {
	int decoded_width = 0, decoded_height = 0;
	uint8_t* rgba = WebPDecodeRGBA(image_vec.data(), image_vec.size(), &decoded_width, &decoded_height);
	if (!rgba) {
		throw std::runtime_error("Image File Error: Failed to decode WEBP image for re-encoding.");
	}

	uint8_t* output = nullptr;
	const std::size_t output_size = WebPEncodeRGBA(
		rgba,
		decoded_width,
		decoded_height,
		decoded_width * 4,
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
	int width = 0, height = 0;

	if (!WebPGetInfo(image_vec.data(), image_vec.size(), &width, &height)) {
		throw std::runtime_error("Error: Not a valid WEBP image.");
	}

	constexpr std::size_t WEBP_EXTENDED_INDEX = 0x0F;
	constexpr std::size_t WEBP_HEADER_LENGTH = 12;

	if (image_vec.size() <= WEBP_EXTENDED_INDEX) {
		throw std::runtime_error("Error: WEBP image too small.");
	}

	if (image_vec[WEBP_EXTENDED_INDEX] == 'X') {
		// Extended format: check for animation.
		constexpr auto ANIM_SIG = std::to_array<Byte>({0x41, 0x4E, 0x49, 0x4D});

		if (searchSig(image_vec, 0, ANIM_SIG).has_value()) {
			throw std::runtime_error("Image File Error: WEBP animation image files not supported.");
		}
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
	image_vec.erase(image_vec.begin(), image_vec.begin() + static_cast<std::ptrdiff_t>(WEBP_HEADER_LENGTH));

	return {width, height};
}
