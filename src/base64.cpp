#include "base64.h"

#include "io_utils.h"

#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>

// Pick the widest available x86 SIMD tier at compile time (the build uses
// -march=native, so this targets the build host). AVX2 (32 bytes/iter) is
// preferred; SSSE3 (16 bytes/iter) is the fallback for pre-AVX2 hosts; otherwise
// scalar. The tiers are mutually exclusive so the unused kernel never compiles.
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#  if defined(__AVX2__)
#    include <immintrin.h>
#    define WBPDV_BASE64_HAS_AVX2  1
#    define WBPDV_BASE64_HAS_SSSE3 0
#  elif defined(__SSSE3__)
#    include <tmmintrin.h>
#    define WBPDV_BASE64_HAS_AVX2  0
#    define WBPDV_BASE64_HAS_SSSE3 1
#  else
#    define WBPDV_BASE64_HAS_AVX2  0
#    define WBPDV_BASE64_HAS_SSSE3 0
#  endif
#else
#  define WBPDV_BASE64_HAS_AVX2  0
#  define WBPDV_BASE64_HAS_SSSE3 0
#endif

namespace {

constexpr int BASE64_VARIANT = sodium_base64_VARIANT_ORIGINAL;
constexpr std::string_view BASE64_ENCODE_TABLE =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// The public helpers append to their destination.  Callers are allowed to
// pass an input span backed by that same vector, so take a copy before any
// resize which could invalidate the span (or make input/output overlap).
[[nodiscard]] bool aliasesVector(std::span<const Byte> input,
                                 const vBytes &destination) noexcept {
  if (input.empty() || destination.empty()) {
    return false;
  }
  const Byte *const input_begin = input.data();
  const Byte *const input_end = input_begin + input.size();
  const Byte *const destination_begin = destination.data();
  const Byte *const destination_end = destination_begin + destination.size();
  const std::less<const Byte *> less;
  return less(input_begin, destination_end) &&
         less(destination_begin, input_end);
}

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

  const std::size_t remaining = binary_data.size() - i;
  if (remaining == 1) {
    const uint32_t triple = static_cast<uint32_t>(binary_data[i]) << 16;
    output[out++] =
        static_cast<Byte>(BASE64_ENCODE_TABLE[(triple >> 18) & 0x3F]);
    output[out++] =
        static_cast<Byte>(BASE64_ENCODE_TABLE[(triple >> 12) & 0x3F]);
    output[out++] = static_cast<Byte>('=');
    output[out] = static_cast<Byte>('=');
  } else if (remaining == 2) {
    const uint32_t triple = (static_cast<uint32_t>(binary_data[i]) << 16) |
                            (static_cast<uint32_t>(binary_data[i + 1]) << 8);
    output[out++] =
        static_cast<Byte>(BASE64_ENCODE_TABLE[(triple >> 18) & 0x3F]);
    output[out++] =
        static_cast<Byte>(BASE64_ENCODE_TABLE[(triple >> 12) & 0x3F]);
    output[out++] =
        static_cast<Byte>(BASE64_ENCODE_TABLE[(triple >> 6) & 0x3F]);
    output[out] = static_cast<Byte>('=');
  }
}

#if WBPDV_BASE64_HAS_AVX2
// AVX2 encode (Wojciech Muła's vector base64): 24 input bytes -> 32 base64 chars
// per iteration. Returns the count of input bytes consumed (a multiple of 24);
// the caller's scalar path finishes the tail. Each iteration loads 32 bytes but
// consumes only 24, so loop_iters is chosen to keep the trailing 8-byte
// over-read inside the input buffer.
[[nodiscard]] std::size_t encodeAvx2(std::span<const Byte> binary_data,
                                     Byte *output) {
  if (binary_data.size() < 32) {
    return 0;
  }
  const std::size_t loop_iters = (binary_data.size() - 32) / 24 + 1;

  // After the permute below each 128-bit lane holds 12 input bytes at positions
  // 0..11; this pshufb spreads them to [b1,b0,b2,b1] quads for sextet extraction.
  const __m256i shuf = _mm256_setr_epi8(
      1, 0, 2, 1, 4, 3, 5, 4, 7, 6, 8, 7, 10, 9, 11, 10,
      1, 0, 2, 1, 4, 3, 5, 4, 7, 6, 8, 7, 10, 9, 11, 10);
  // Per-category ASCII offset LUT: upper +65, lower +71, digits -4, '+' -19, '/' -16.
  const __m256i ascii_lut = _mm256_setr_epi8(
      65, 71, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -19, -16, 0, 0,
      65, 71, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -19, -16, 0, 0);

  const Byte *in_p = binary_data.data();
  Byte *out_p = output;

  for (std::size_t i = 0; i < loop_iters; ++i) {
    __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(in_p));
    in = _mm256_permutevar8x32_epi32(in,
                                     _mm256_setr_epi32(0, 1, 2, 2, 3, 4, 5, 5));
    const __m256i spread = _mm256_shuffle_epi8(in, shuf);

    // Sextet extraction via shift-by-multiply (Muła's constants).
    const __m256i t0 = _mm256_and_si256(spread, _mm256_set1_epi32(0x0FC0FC00));
    const __m256i t1 = _mm256_mulhi_epu16(t0, _mm256_set1_epi32(0x04000040));
    const __m256i t2 = _mm256_and_si256(spread, _mm256_set1_epi32(0x003F03F0));
    const __m256i t3 = _mm256_mullo_epi16(t2, _mm256_set1_epi32(0x01000010));
    const __m256i sextets = _mm256_or_si256(t1, t3);

    // sextet -> ASCII via category LUT.
    const __m256i sat = _mm256_subs_epu8(sextets, _mm256_set1_epi8(51));
    const __m256i mask = _mm256_cmpgt_epi8(sextets, _mm256_set1_epi8(25));
    const __m256i cat = _mm256_sub_epi8(sat, mask); // sub of -1 == +1
    const __m256i ofs = _mm256_shuffle_epi8(ascii_lut, cat);
    const __m256i ascii = _mm256_add_epi8(sextets, ofs);

    _mm256_storeu_si256(reinterpret_cast<__m256i *>(out_p), ascii);

    in_p += 24;
    out_p += 32;
  }

  return loop_iters * 24;
}
#endif

#if WBPDV_BASE64_HAS_SSSE3
[[nodiscard]] __m128i encodeSextetsToAscii(__m128i sextets) {
  const __m128i offsets =
      _mm_setr_epi8(65, 71, -4, -19, -16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  __m128i indices = _mm_setzero_si128();
  indices = _mm_sub_epi8(indices, _mm_cmpgt_epi8(sextets, _mm_set1_epi8(25)));
  indices = _mm_sub_epi8(indices, _mm_cmpgt_epi8(sextets, _mm_set1_epi8(51)));
  indices = _mm_sub_epi8(indices, _mm_cmpgt_epi8(sextets, _mm_set1_epi8(61)));
  indices = _mm_sub_epi8(indices, _mm_cmpgt_epi8(sextets, _mm_set1_epi8(62)));
  return _mm_add_epi8(sextets, _mm_shuffle_epi8(offsets, indices));
}

[[nodiscard]] __m128i unpackTriplesToSextets(__m128i input) {
  const __m128i shuffle =
      _mm_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1, 8, 7, 6, -1, 11, 10, 9, -1);
  const __m128i mask = _mm_set1_epi32(0x3F);
  const __m128i triples = _mm_shuffle_epi8(input, shuffle);
  const __m128i sextet0 = _mm_and_si128(_mm_srli_epi32(triples, 18), mask);
  const __m128i sextet1 =
      _mm_slli_epi32(_mm_and_si128(_mm_srli_epi32(triples, 12), mask), 8);
  const __m128i sextet2 =
      _mm_slli_epi32(_mm_and_si128(_mm_srli_epi32(triples, 6), mask), 16);
  const __m128i sextet3 = _mm_slli_epi32(_mm_and_si128(triples, mask), 24);
  return _mm_or_si128(_mm_or_si128(sextet0, sextet1),
                      _mm_or_si128(sextet2, sextet3));
}

[[nodiscard]] std::size_t encodeSsse3(std::span<const Byte> binary_data,
                                      Byte *output) {
  std::size_t input_offset = 0;
  std::size_t output_offset = 0;
  for (; binary_data.size() - input_offset >= 16;
       input_offset += 12, output_offset += 16) {
    const __m128i input = _mm_loadu_si128(
        reinterpret_cast<const __m128i *>(binary_data.data() + input_offset));
    const __m128i ascii = encodeSextetsToAscii(unpackTriplesToSextets(input));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(output + output_offset),
                     ascii);
  }
  return input_offset;
}
#endif

} // namespace

void binaryToBase64(std::span<const Byte> binary_data, vBytes &output_vec) {
  if (binary_data.empty()) {
    return;
  }

  vBytes stable_input;
  if (aliasesVector(binary_data, output_vec)) {
    stable_input.assign(binary_data.begin(), binary_data.end());
    binary_data = byteSpan(stable_input);
  }

  const std::size_t output_size = encodedBytes(binary_data.size());
  const std::size_t base_offset = output_vec.size();
  if (base_offset > std::numeric_limits<std::size_t>::max() - output_size) {
    throw std::overflow_error("Base64 encode destination size overflow.");
  }

  output_vec.resize(base_offset + output_size);
  Byte *const output = output_vec.data() + base_offset;

  std::size_t processed_input = 0;
#if WBPDV_BASE64_HAS_AVX2
  processed_input = encodeAvx2(binary_data, output);
#elif WBPDV_BASE64_HAS_SSSE3
  processed_input = encodeSsse3(binary_data, output);
#endif

  encodeScalar(binary_data.subspan(processed_input),
               output + ((processed_input / 3) * 4));
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

  vBytes stable_input;
  if (aliasesVector(base64_data, destination_vec)) {
    stable_input.assign(base64_data.begin(), base64_data.end());
    base64_data = byteSpan(stable_input);
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
