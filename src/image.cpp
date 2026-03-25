#include "image.h"
#include "io_utils.h"

#include <webp/decode.h>
#include <webp/encode.h>

#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace {
constexpr std::size_t WEBP_HEADER_LENGTH = 12;
constexpr auto RIFF_TAG = std::to_array<Byte>({'R', 'I', 'F', 'F'});
constexpr auto WEBP_TAG = std::to_array<Byte>({'W', 'E', 'B', 'P'});
constexpr auto VP8_TAG  = std::to_array<Byte>({'V', 'P', '8', ' '});

[[nodiscard]] std::uint32_t readLe32(std::span<const Byte> data, std::size_t offset) {
	requireSpanRange(data, offset, 4, "Internal Error: image.cpp readLe32 out of bounds.");
	return static_cast<std::uint32_t>(data[offset]) |
		   (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
		   (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
		   (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

// Fast path for already-lossy covers: strip the RIFF wrapper and any metadata,
// then keep only the raw VP8 chunk used by the output container.
[[nodiscard]] bool extractLossyVp8Chunk(std::span<const Byte> webp_data, vBytes& vp8_chunk) {
	if (!spanHasRange(webp_data, 0, WEBP_HEADER_LENGTH)) {
		return false;
	}
	if (!std::equal(RIFF_TAG.begin(), RIFF_TAG.end(), webp_data.begin()) ||
		!std::equal(WEBP_TAG.begin(), WEBP_TAG.end(), webp_data.begin() + 8)) {
		return false;
	}

	const std::size_t riff_payload_size = readLe32(webp_data, 4);
	if (riff_payload_size < WEBP_TAG.size()) {
		return false;
	}

	const std::size_t riff_total_size = 8 + riff_payload_size;
	if (!spanHasRange(webp_data, 0, riff_total_size)) {
		return false;
	}

	for (std::size_t offset = WEBP_HEADER_LENGTH; offset < riff_total_size; ) {
		if (!spanHasRange(webp_data, offset, 8)) {
			return false;
		}

		const std::uint32_t chunk_size = readLe32(webp_data, offset + 4);
		const std::size_t padded_chunk_size = static_cast<std::size_t>(chunk_size) + ((chunk_size & 1U) ? 1U : 0U);
		const std::size_t total_chunk_bytes = 8 + padded_chunk_size;
		if (!spanHasRange(webp_data, offset, total_chunk_bytes)) {
			return false;
		}

		if (std::equal(VP8_TAG.begin(), VP8_TAG.end(), webp_data.begin() + static_cast<std::ptrdiff_t>(offset))) {
			vp8_chunk.assign(
				webp_data.begin() + static_cast<std::ptrdiff_t>(offset),
				webp_data.begin() + static_cast<std::ptrdiff_t>(offset + total_chunk_bytes)
			);
			return true;
		}

		offset += total_chunk_bytes;
	}

	return false;
}

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

	if (features.format == 1 && features.has_alpha == 0) {
		vBytes vp8_chunk;
		if (extractLossyVp8Chunk(std::span<const Byte>(image_vec.data(), image_vec.size()), vp8_chunk)) {
			image_vec = std::move(vp8_chunk);
			return {features.width, features.height};
		}
	}

	// Fallback path: normalize to a fresh lossy WebP to minimize cover image size.
	// This also strips all existing metadata (EXIF, XMP, ICCP) and
	// converts lossless images to lossy when the fast path above cannot be used.
	constexpr float ENCODE_QUALITY = 75.0f;
	reEncodeAsLossy(image_vec, ENCODE_QUALITY);

	// The re-encoded image is a simple-format WebP (RIFF + VP8).
	// Strip the 12-byte RIFF/WEBP header to leave just the VP8 chunk.
	if (image_vec.size() < WEBP_HEADER_LENGTH) {
		throw std::runtime_error("Error: Re-encoded WEBP image too small.");
	}
	if (!std::equal(RIFF_TAG.begin(), RIFF_TAG.end(), image_vec.begin()) ||
		!std::equal(WEBP_TAG.begin(), WEBP_TAG.end(), image_vec.begin() + 8)) {
		throw std::runtime_error("Image File Error: Re-encoded WEBP image has an invalid container header.");
	}
	image_vec.erase(image_vec.begin(), image_vec.begin() + static_cast<std::ptrdiff_t>(WEBP_HEADER_LENGTH));

	return {features.width, features.height};
}
