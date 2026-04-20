#include "compression.h"
#include <algorithm>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <zlib.h>
namespace {

constexpr std::size_t ZLIB_BUFSIZE = 2 * 1024 * 1024;
constexpr std::size_t MAX_INFLATED_OUTPUT_SIZE = MAX_PROGRAM_FILE_SIZE;
constexpr std::size_t MIN_INFLATE_INITIAL_RESERVE = 256 * 1024;
constexpr std::size_t MAX_INFLATE_INITIAL_RESERVE = 64ULL * 1024 * 1024;

struct ZlibGuard {
  z_stream strm{};
  bool inflate_mode{};

  explicit ZlibGuard(bool use_inflate, int level = Z_BEST_COMPRESSION) : inflate_mode(use_inflate) {
    const int rc = inflate_mode ? inflateInit(&strm) : deflateInit(&strm, level);
    if (rc != Z_OK) {
      throw std::runtime_error(inflate_mode ? "zlib inflateInit failed." : "zlib deflateInit failed.");
    }
  }
  ~ZlibGuard() {
    inflate_mode ? inflateEnd(&strm) : deflateEnd(&strm);
  }
  ZlibGuard(const ZlibGuard&) = delete;
  ZlibGuard& operator=(const ZlibGuard&) = delete;
};

void appendOutputChunk(vBytes& result, std::span<const Byte> buffer, std::size_t written, std::size_t max_output_size) {
  if (written == 0)
    return;
  if (written > max_output_size || result.size() > max_output_size - written)
    throw std::runtime_error("Zlib Compression Error: Inflated data exceeds maximum program size limit.");
  result.insert(result.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
}

[[nodiscard]] std::size_t inflateReserveHint(std::size_t input_size, std::size_t max_output_size) {
  // Reserve enough room to avoid repeated growth on typical inputs without
  // overcommitting memory for hostile or heavily-expanded streams.
  const std::size_t cap = std::min(max_output_size, MAX_INFLATE_INITIAL_RESERVE);
  if (cap == 0)
    return 0;
  const std::size_t hint = std::clamp(input_size, MIN_INFLATE_INITIAL_RESERVE, cap);
  return hint <= cap / 2 ? hint * 2 : hint;
}

[[nodiscard]] vBytes inflateToVector(std::span<const Byte> input, std::size_t max_output_size) {
  ZlibGuard guard(true);
  z_stream& strm = guard.strm;
  vBytes buffer(ZLIB_BUFSIZE);
  vBytes result;
  result.reserve(inflateReserveHint(input.size(), max_output_size));
  std::size_t input_offset = 0;
  auto refill_input = [&] {
    if (strm.avail_in != 0 || input_offset >= input.size()) {
      return;
    }
    const std::size_t chunk_size =
        std::min<std::size_t>(input.size() - input_offset, static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
    strm.next_in = const_cast<Byte*>(input.data() + static_cast<std::ptrdiff_t>(input_offset));
    strm.avail_in = static_cast<uInt>(chunk_size);
    input_offset += chunk_size;
  };
  auto flush_output = [&] {
    const std::size_t written = ZLIB_BUFSIZE - strm.avail_out;
    appendOutputChunk(result, std::span<const Byte>(buffer.data(), written), written, max_output_size);
    strm.next_out = buffer.data();
    strm.avail_out = static_cast<uInt>(ZLIB_BUFSIZE);
  };
  while (true) {
    refill_input();
    strm.next_out = buffer.data();
    strm.avail_out = static_cast<uInt>(ZLIB_BUFSIZE);
    const int ret = inflate(&strm, Z_NO_FLUSH);
    flush_output();
    if (ret == Z_STREAM_END)
      break;
    if (ret == Z_OK)
      continue;
    if (ret == Z_BUF_ERROR) {
      if (strm.avail_in == 0) {
        refill_input();
        if (strm.avail_in != 0) {
          continue;
        }
        // Input exhausted without Z_STREAM_END: the stream is truncated.
        throw std::runtime_error("zlib inflate failed: truncated stream (Z_STREAM_END not reached).");
      }
      if (strm.avail_out == 0) {
        continue;
      }
      throw std::runtime_error("zlib inflate failed: stalled stream.");
    }
    throw std::runtime_error(std::format("zlib inflate failed: {}", strm.msg ? strm.msg : std::to_string(ret)));
  }
  return result;
}
} // namespace

void zlibDeflate(vBytes& data_vec) {
  ZlibGuard guard(false);
  z_stream& strm = guard.strm;
  vBytes buffer(ZLIB_BUFSIZE), result;
  const uLong bound =
      deflateBound(&strm, static_cast<uLong>(std::min(data_vec.size(), static_cast<std::size_t>(std::numeric_limits<uLong>::max()))));
  if (bound <= std::numeric_limits<std::size_t>::max())
    result.reserve(static_cast<std::size_t>(bound));
  std::size_t input_offset = 0;
  auto refill_input = [&] {
    if (strm.avail_in != 0 || input_offset >= data_vec.size())
      return;
    const std::size_t chunk_size = std::min<std::size_t>(data_vec.size() - input_offset, std::numeric_limits<uInt>::max());
    strm.next_in = data_vec.data() + input_offset;
    strm.avail_in = static_cast<uInt>(chunk_size);
    input_offset += chunk_size;
  };
  auto flush_output = [&] {
    const std::size_t written = ZLIB_BUFSIZE - strm.avail_out;
    appendOutputChunk(result, buffer, written, std::numeric_limits<std::size_t>::max());
    strm.next_out = buffer.data();
    strm.avail_out = ZLIB_BUFSIZE;
  };
  refill_input();
  while (true) {
    strm.next_out = buffer.data();
    strm.avail_out = ZLIB_BUFSIZE;
    int ret = deflate(&strm, (input_offset >= data_vec.size() && strm.avail_in == 0) ? Z_FINISH : Z_NO_FLUSH);
    if (ret == Z_STREAM_ERROR)
      throw std::runtime_error("zlib deflate failed.");
    flush_output();
    if (ret == Z_STREAM_END)
      break;
    if (strm.avail_in == 0)
      refill_input();
  }
  data_vec = std::move(result);
}

void zlibInflate(vBytes& data_vec) {
  data_vec = inflateToVector(data_vec, MAX_INFLATED_OUTPUT_SIZE);
}
