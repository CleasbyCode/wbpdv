#include "base64.h"

#include <limits>
#include <stdexcept>

namespace {

constexpr int BASE64_VARIANT = sodium_base64_VARIANT_ORIGINAL;

[[nodiscard]] std::size_t encodedBytes(std::size_t input_size) {
	const std::size_t encoded_with_nul = sodium_base64_ENCODED_LEN(input_size, BASE64_VARIANT);
	if (encoded_with_nul == 0) {
		throw std::overflow_error("Base64 encode size overflow.");
	}
	return encoded_with_nul - 1;
}

} // namespace

void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec) {
	if (binary_data.empty()) {
		return;
	}

	const std::size_t output_size = encodedBytes(binary_data.size());
	const std::size_t base_offset = output_vec.size();
	if (base_offset > std::numeric_limits<std::size_t>::max() - (output_size + 1)) {
		throw std::overflow_error("Base64 encode destination size overflow.");
	}

	output_vec.resize(base_offset + output_size + 1);
	sodium_bin2base64(
		reinterpret_cast<char*>(output_vec.data() + static_cast<std::ptrdiff_t>(base_offset)),
		output_size + 1,
		binary_data.data(),
		binary_data.size(),
		BASE64_VARIANT
	);
	output_vec.pop_back();
}

void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec) {
	const std::size_t input_size = base64_data.size();
	if (input_size == 0 || (input_size % 4) != 0) {
		throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty.");
	}

	const std::size_t decoded_upper_bound = (input_size / 4) * 3;
	const std::size_t base_offset = destination_vec.size();
	if (base_offset > std::numeric_limits<std::size_t>::max() - decoded_upper_bound) {
		throw std::overflow_error("Base64 decode destination size overflow.");
	}

	destination_vec.resize(base_offset + decoded_upper_bound);
	std::size_t decoded_size = 0;
	const int rc = sodium_base642bin(
		destination_vec.data() + static_cast<std::ptrdiff_t>(base_offset),
		decoded_upper_bound,
		reinterpret_cast<const char*>(base64_data.data()),
		base64_data.size(),
		nullptr,
		&decoded_size,
		nullptr,
		BASE64_VARIANT
	);
	if (rc != 0) {
		destination_vec.resize(base_offset);
		throw std::invalid_argument("Invalid Base64 character encountered.");
	}
	destination_vec.resize(base_offset + decoded_size);
}
