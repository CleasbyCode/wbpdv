#include "compression.h"

#include "io_utils.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <libdeflate.h>
#include <zlib.h>
namespace {

constexpr std::size_t ZLIB_BUFSIZE = 2 * 1024 * 1024;
constexpr std::size_t MIN_INFLATE_INITIAL_RESERVE = 256 * 1024;
constexpr std::size_t MAX_INFLATE_INITIAL_RESERVE = 64ULL * 1024 * 1024;

// Inputs at or below this size are deflated in a single whole-buffer libdeflate
// call (peak RSS ~= input + bound, ~2x input). Larger inputs use the zlib
// streaming path below, which holds only fixed-size chunks. Both emit a
// standard RFC 1950 zlib stream, so the recover-side zlib inflate decodes
// either one.
constexpr std::size_t LIBDEFLATE_WHOLE_BUFFER_LIMIT = 256ULL * 1024 * 1024;

struct LibdeflateCompressorGuard {
  libdeflate_compressor *c{nullptr};
  explicit LibdeflateCompressorGuard(int level)
      : c(libdeflate_alloc_compressor(level)) {}
  ~LibdeflateCompressorGuard() {
    if (c) {
      libdeflate_free_compressor(c);
    }
  }
  LibdeflateCompressorGuard(const LibdeflateCompressorGuard &) = delete;
  LibdeflateCompressorGuard &operator=(const LibdeflateCompressorGuard &) = delete;
};

struct ZlibGuard {
  z_stream strm{};
  bool inflate_mode{};

  explicit ZlibGuard(bool use_inflate, int level = Z_BEST_COMPRESSION)
      : inflate_mode(use_inflate) {
    const int rc =
        inflate_mode ? inflateInit(&strm) : deflateInit(&strm, level);
    if (rc != Z_OK) {
      throw std::runtime_error(inflate_mode ? "zlib inflateInit failed."
                                            : "zlib deflateInit failed.");
    }
  }
  ~ZlibGuard() {
    if (inflate_mode) {
      inflateEnd(&strm);
    } else {
      deflateEnd(&strm);
    }
  }

  ZlibGuard(const ZlibGuard &) = delete;
  ZlibGuard &operator=(const ZlibGuard &) = delete;
};

struct VectorWipeGuard {
  vBytes *bytes{};
  bool active{true};

  explicit VectorWipeGuard(vBytes &guarded_bytes) : bytes(&guarded_bytes) {}
  VectorWipeGuard(const VectorWipeGuard &) = delete;
  VectorWipeGuard &operator=(const VectorWipeGuard &) = delete;

  ~VectorWipeGuard() {
    wipe();
  }

  void wipe() noexcept {
    if (active && bytes != nullptr && !bytes->empty()) {
      sodium_memzero(bytes->data(), bytes->size());
      active = false;
    }
  }

  void release() noexcept { active = false; }
};

void appendOutputChunk(vBytes &result, std::span<const Byte> chunk,
                       std::size_t max_output_size,
                       std::string_view limit_error) {
  if (chunk.empty()) {
    return;
  }
  if (chunk.size() > max_output_size ||
      result.size() > max_output_size - chunk.size()) {
    throw std::runtime_error(std::string(limit_error));
  }
  const std::size_t offset = result.size();
  result.resize(offset + chunk.size());
  std::memcpy(result.data() + static_cast<std::ptrdiff_t>(offset),
              chunk.data(), chunk.size());
}

bool refillInput(z_stream &strm, std::span<const Byte> input,
                 std::size_t &input_offset) {
  if (strm.avail_in != 0) {
    return true;
  }
  if (input_offset >= input.size()) {
    return false;
  }
  const std::size_t chunk_size = std::min<std::size_t>(
      input.size() - input_offset, std::numeric_limits<uInt>::max());
  strm.next_in = const_cast<Byte *>(input.data() +
                                    static_cast<std::ptrdiff_t>(input_offset));
  strm.avail_in = static_cast<uInt>(chunk_size);
  input_offset += chunk_size;
  return true;
}

void resetOutput(z_stream &strm, std::span<Byte> buffer) {
  strm.next_out = buffer.data();
  strm.avail_out = static_cast<uInt>(buffer.size());
}

[[nodiscard]] std::span<const Byte> writtenOutput(std::span<const Byte> buffer,
                                                  const z_stream &strm) {
  return buffer.first(buffer.size() - strm.avail_out);
}

void flushOutput(z_stream &strm, vBytes &result, std::span<Byte> buffer,
                 std::size_t max_output_size, std::string_view limit_error) {
  appendOutputChunk(result, writtenOutput(buffer, strm), max_output_size,
                    limit_error);
  resetOutput(strm, buffer);
}

[[nodiscard]] std::size_t inflateReserveHint(std::size_t input_size,
                                             std::size_t max_output_size) {
  // Reserve enough room to avoid repeated growth on typical inputs without
  // overcommitting memory for hostile or heavily-expanded streams.
  const std::size_t cap =
      std::min(max_output_size, MAX_INFLATE_INITIAL_RESERVE);
  if (cap == 0) {
    return 0;
  }
  if (cap <= MIN_INFLATE_INITIAL_RESERVE) {
    return cap;
  }
  const std::size_t hint =
      std::clamp(input_size, MIN_INFLATE_INITIAL_RESERVE, cap);
  return hint <= cap / 2 ? hint * 2 : hint;
}

[[nodiscard]] vBytes inflateToVector(std::span<const Byte> input,
                                     std::size_t max_output_size) {
  ZlibGuard guard(true);
  z_stream &strm = guard.strm;
  vBytes buffer(ZLIB_BUFSIZE);
  VectorWipeGuard buffer_guard(buffer);
  vBytes result;
  VectorWipeGuard result_guard(result);
  result.reserve(inflateReserveHint(input.size(), max_output_size));
  std::size_t input_offset = 0;
  auto flush_output = [&] {
    flushOutput(strm, result, byteSpan(buffer), max_output_size,
                "Zlib Compression Error: Inflated data exceeds maximum program "
                "size limit.");
  };
  while (true) {
    refillInput(strm, input, input_offset);
    resetOutput(strm, buffer);
    const int ret = inflate(&strm, Z_NO_FLUSH);
    const bool output_was_full = (strm.avail_out == 0);
    flush_output();
    if (ret == Z_STREAM_END) {
      // A payload is exactly one zlib stream.  Accepting bytes after the end
      // would make authenticated-but-malformed payloads ambiguous and would
      // silently accept concatenated streams while decoding only the first.
      if (strm.avail_in != 0 || input_offset != input.size()) {
        throw std::runtime_error(
            "zlib inflate failed: trailing data after stream end.");
      }
      break;
    }
    if (ret == Z_OK) {
      continue;
    }
    if (ret == Z_BUF_ERROR) {
      if (strm.avail_in == 0) {
        if (refillInput(strm, input, input_offset)) {
          continue;
        }
        // Input exhausted without Z_STREAM_END: the stream is truncated.
        throw std::runtime_error("zlib inflate failed: truncated stream "
                                 "(Z_STREAM_END not reached).");
      }
      if (output_was_full) {
        continue;
      }
      throw std::runtime_error("zlib inflate failed: stalled stream.");
    }
    throw std::runtime_error(std::format(
        "zlib inflate failed: {}", strm.msg ? strm.msg : std::to_string(ret)));
  }
  result_guard.release();
  return result;
}
} // namespace

void zlibDeflate(vBytes &data_vec, std::size_t max_output_size) {
  VectorWipeGuard input_guard(data_vec);

  // libdeflate whole-buffer fast path. data_vec is already fully in memory, so
  // this is a clean drop-in. Measured on real corpora, libdeflate L9 costs
  // ~2.6-2.9x the time of L6 for only ~1-2% smaller output (L10-12 are far
  // worse), so the payload is compressed at L6 — the ratio/time sweet spot.
  // Do NOT raise this back toward 9-12 without re-measuring.
  if (data_vec.size() <= LIBDEFLATE_WHOLE_BUFFER_LIMIT) {
    LibdeflateCompressorGuard compressor(6);
    if (!compressor.c) {
      throw std::runtime_error("libdeflate: failed to allocate compressor.");
    }
    const std::size_t bound =
        libdeflate_zlib_compress_bound(compressor.c, data_vec.size());
    if (max_output_size == 0) {
      throw std::runtime_error("Zlib Compression Error: Compressed data exceeds "
                               "maximum output size limit.");
    }
    // libdeflate accepts a destination smaller than its worst-case bound and
    // reports zero if the actual stream does not fit.  Capping the allocation
    // here is important for small-output modes: highly compressible large
    // inputs should not transiently allocate an input-sized bound buffer.
    const std::size_t destination_capacity =
        std::min(bound, max_output_size);
    vBytes result(destination_capacity);
    VectorWipeGuard result_guard(result);
    const std::size_t produced = libdeflate_zlib_compress(
        compressor.c, data_vec.data(), data_vec.size(), result.data(),
        destination_capacity);
    if (produced == 0) {
      if (destination_capacity < bound) {
        throw std::runtime_error(
            "Zlib Compression Error: Compressed data exceeds maximum output "
            "size limit.");
      }
      throw std::runtime_error("libdeflate: zlib compression failed.");
    }
    if (produced > max_output_size) {
      throw std::runtime_error("Zlib Compression Error: Compressed data exceeds "
                               "maximum output size limit.");
    }
    result.resize(produced);
    input_guard.wipe();        // scrub the plaintext input before releasing it
    input_guard.release();
    data_vec = std::move(result);
    result_guard.release();    // result is now the (non-sensitive) output
    return;
  }

  // Level 6 (Z_DEFAULT_COMPRESSION), consistent with the libdeflate fast path
  // above: level 9 costs far more time for ~1-2% ratio on the >256 MiB path.
  ZlibGuard guard(false, Z_DEFAULT_COMPRESSION);
  z_stream &strm = guard.strm;
  vBytes buffer(ZLIB_BUFSIZE), result;
  VectorWipeGuard buffer_guard(buffer);
  VectorWipeGuard result_guard(result);
  const uLong bound = deflateBound(
      &strm, static_cast<uLong>(std::min(
                 data_vec.size(),
                 static_cast<std::size_t>(std::numeric_limits<uLong>::max()))));
  if (bound <= std::numeric_limits<std::size_t>::max()) {
    result.reserve(std::min(static_cast<std::size_t>(bound), max_output_size));
  }
  std::size_t input_offset = 0;
  auto flush_output = [&] {
    flushOutput(strm, result, byteSpan(buffer), max_output_size,
                "Zlib Compression Error: Compressed data exceeds maximum "
                "output size limit.");
  };
  refillInput(strm, byteSpan(data_vec), input_offset);
  while (true) {
    resetOutput(strm, buffer);
    int ret =
        deflate(&strm, (input_offset >= data_vec.size() && strm.avail_in == 0)
                           ? Z_FINISH
                           : Z_NO_FLUSH);
    if (ret == Z_STREAM_ERROR) {
      throw std::runtime_error("zlib deflate failed.");
    }
    flush_output();
    if (ret == Z_STREAM_END) {
      break;
    }
    if (strm.avail_in == 0) {
      refillInput(strm, byteSpan(data_vec), input_offset);
    }
  }
  input_guard.wipe();
  input_guard.release();
  data_vec = std::move(result);
  result_guard.release();
}

void zlibInflate(vBytes &data_vec) {
  VectorWipeGuard input_guard(data_vec);
  vBytes inflated = inflateToVector(data_vec, MAX_PROGRAM_FILE_SIZE);
  VectorWipeGuard inflated_guard(inflated);
  input_guard.wipe();
  input_guard.release();
  data_vec = std::move(inflated);
  inflated_guard.release();
}
