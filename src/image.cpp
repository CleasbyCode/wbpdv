#include "image.h"

#include <webp/decode.h>

#include <algorithm>
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
		// Extended format: check for animation, strip XMP/EXIF, find VP8 chunk.
		constexpr auto ANIM_SIG = std::to_array<Byte>({0x41, 0x4E, 0x49, 0x4D});
		constexpr auto XMP_SIG  = std::to_array<Byte>({0x58, 0x4D, 0x50, 0x20});
		constexpr auto EXIF_SIG = std::to_array<Byte>({0x45, 0x58, 0x49, 0x46});
		constexpr auto VP8_SIG  = std::to_array<Byte>({0x56, 0x50, 0x38});

		if (searchSig(image_vec, 0, ANIM_SIG).has_value()) {
			throw std::runtime_error("Image File Error: WEBP animation image files not supported.");
		}

		if (auto xmp_pos = searchSig(image_vec, 0, XMP_SIG)) {
			image_vec.erase(image_vec.begin() + static_cast<std::ptrdiff_t>(*xmp_pos), image_vec.end());
		}

		if (auto exif_pos = searchSig(image_vec, 0, EXIF_SIG)) {
			image_vec.erase(image_vec.begin() + static_cast<std::ptrdiff_t>(*exif_pos), image_vec.end());
		}

		auto vp8_pos = searchSig(image_vec, WEBP_EXTENDED_INDEX, VP8_SIG);
		if (!vp8_pos.has_value()) {
			throw std::runtime_error("Error: Cannot find VP8 chunk in WEBP image.");
		}

		image_vec.erase(image_vec.begin(), image_vec.begin() + static_cast<std::ptrdiff_t>(*vp8_pos));
	} else {
		// Simple format: erase the first 12 bytes (RIFF/WEBP header).
		if (image_vec.size() < WEBP_HEADER_LENGTH) {
			throw std::runtime_error("Error: WEBP image too small.");
		}
		image_vec.erase(image_vec.begin(), image_vec.begin() + static_cast<std::ptrdiff_t>(WEBP_HEADER_LENGTH));
	}

	return {width, height};
}
