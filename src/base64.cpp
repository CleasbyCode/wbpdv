#include "base64.h"

#include "io_utils.h"

#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

constexpr int BASE64_VARIANT = sodium_base64_VARIANT_ORIGINAL;
constexpr std::string_view BASE64_ENCODE_TABLE =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] std::size_t encodedBytes(std::size_t input_size) {
  constexpr std::size_t MAX_ENCODED_BYTES =
      std::numeric_limits<std::size_t>::max() - 1;
  if (input_size > (MAX_ENCODED_BYTES / 4) * 3) {
    throw std::overflow_error("Base64 encode size overflow.");
  }
  return ((input_size + 2) / 3) * 4;
}

[[nodiscard]] std::size_t decodedBytesBound(std::span<const Byte> base64_data) {
  const std::size_t upper_bound =
      checkedMulSize(base64_data.size() / 4, 3, "Base64 decode size overflow.");
  std::size_t padding = 0;
  if (!base64_data.empty() && base64_data.back() == static_cast<Byte>('=')) {
    ++padding;
    if (base64_data.size() >= 2 &&
        base64_data[base64_data.size() - 2] == static_cast<Byte>('=')) {
      ++padding;
    }
  }
  return upper_bound - padding;
}

void encodeScalar(std::span<const Byte> binary_data, Byte *output) {
  const std::size_t full_groups_size = (binary_data.size() / 3) * 3;
  std::size_t out = 0;
  std::size_t i = 0;
  for (; i < full_groups_size; i += 3) {
    const Byte a = binary_data[i];
    const Byte b = binary_data[i + 1];
    const Byte c = binary_data[i + 2];
    const uint32_t triple = (static_cast<uint32_t>(a) << 16) |
                            (static_cast<uint32_t>(b) << 8) |
                            static_cast<uint32_t>(c);
    output[out++] =
        static_cast<Byte>(BASE64_ENCODE_TABLE[(triple >> 18) & 0x3F]);
    output[out++] =
        static_cast<Byte>(BASE64_ENCODE_TABLE[(triple >> 12) & 0x3F]);
    output[out++] =
        static_cast<Byte>(BASE64_ENCODE_TABLE[(triple >> 6) & 0x3F]);
    output[out++] = static_cast<Byte>(BASE64_ENCODE_TABLE[triple & 0x3F]);
  }

  // Tail of 1 or 2 bytes: emit (remaining + 1) sextets, pad the rest with '='.
  const std::size_t remaining = binary_data.size() - i;
  if (remaining != 0) {
    uint32_t triple = 0;
    for (std::size_t k = 0; k < remaining; ++k) {
      triple |= static_cast<uint32_t>(binary_data[i + k]) << (16 - 8 * k);
    }
    for (std::size_t k = 0; k < 4; ++k) {
      output[out++] =
          k <= remaining
              ? static_cast<Byte>(BASE64_ENCODE_TABLE[(triple >> (18 - 6 * k)) &
                                                      0x3F])
              : static_cast<Byte>('=');
    }
  }
}

} // namespace

void binaryToBase64(std::span<const Byte> binary_data, vBytes &output_vec) {
  if (binary_data.empty()) {
    return;
  }

  const std::size_t output_size = encodedBytes(binary_data.size());
  const std::size_t base_offset = output_vec.size();
  if (base_offset > std::numeric_limits<std::size_t>::max() - output_size) {
    throw std::overflow_error("Base64 encode destination size overflow.");
  }

  output_vec.resize(base_offset + output_size);  encodeScalar(binary_data, output_vec.data() + base_offset);
}

void appendBase64AsBinary(std::span<const Byte> base64_data,
                          vBytes &destination_vec,
                          std::size_t max_decoded_append_size) {
  const std::size_t input_size = base64_data.size();
  if (input_size == 0 || (input_size % 4) != 0) {
    throw std::invalid_argument(
        "Base64 input size must be a multiple of 4 and non-empty.");
  }
  const std::size_t decoded_bound = decodedBytesBound(base64_data);
  if (decoded_bound > max_decoded_append_size) {
    throw std::overflow_error(
        "Base64 decoded data exceeds maximum allowed size.");
  }

  const std::size_t base_offset = destination_vec.size();
  if (base_offset > std::numeric_limits<std::size_t>::max() - decoded_bound) {
    throw std::overflow_error("Base64 decode destination size overflow.");
  }
  destination_vec.resize(base_offset + decoded_bound);
  auto restore_destination = [&] {
    if (destination_vec.size() > base_offset) {
      sodium_memzero(destination_vec.data() + base_offset,
                     destination_vec.size() - base_offset);
      destination_vec.resize(base_offset);
    }
  };
  std::size_t decoded_size = 0;
  if (sodium_base642bin(destination_vec.data() + base_offset, decoded_bound,
                        reinterpret_cast<const char *>(base64_data.data()),
                        base64_data.size(), nullptr, &decoded_size, nullptr,
                        BASE64_VARIANT) != 0) {
    restore_destination();
    throw std::invalid_argument("Invalid Base64 character encountered.");
  }
  if (decoded_size > max_decoded_append_size) {
    restore_destination();
    throw std::overflow_error(
        "Base64 decoded data exceeds maximum allowed size.");
  }
  destination_vec.resize(base_offset + decoded_size);
}
