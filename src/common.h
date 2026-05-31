#pragma once

// ---------------------------------------------------------------------------
// Toolchain floor. wbpdv is distributed as source and built by each user, so
// fail early with a clear message rather than a wall of template errors when
// the compiler is too old. Features that set this floor:
//   - std::print / std::println          (GCC 14, Clang 18 + libc++ 18)
//   - std::format, std::ranges, [[assume]] (GCC 13+ / Clang 19)
// A matching C++23 standard library is required.
// ---------------------------------------------------------------------------
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 14
#  error "wbpdv requires GCC >= 14 (for std::print/std::format). Please upgrade your compiler."
#elif defined(__clang__) && __clang_major__ < 18
#  error "wbpdv requires Clang >= 18 with a C++23 standard library (libc++ 18+ or libstdc++ 14+). Please upgrade your compiler."
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <sodium.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using Byte = std::uint8_t;

// Byte buffers in this tool are normally filled by syscalls or codecs
// immediately after allocation; default-initializing avoids redundant zeroing.
template <class T> struct DefaultInitAllocator : std::allocator<T> {
  using value_type = T;

  DefaultInitAllocator() noexcept = default;

  template <class U>
  DefaultInitAllocator(const DefaultInitAllocator<U> &) noexcept {}

  template <class U> struct rebind {
    using other = DefaultInitAllocator<U>;
  };

  template <class U>
  void construct(U *ptr) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void *>(ptr)) U;
  }

  template <class U, class... Args>
    requires(sizeof...(Args) > 0)
  void construct(U *ptr, Args &&...args) {
    std::construct_at(ptr, std::forward<Args>(args)...);
  }
};

template <class T, class U>
inline bool operator==(const DefaultInitAllocator<T> &,
                       const DefaultInitAllocator<U> &) noexcept {
  return true;
}

using vBytes = std::vector<Byte, DefaultInitAllocator<Byte>>;
using Key = std::array<Byte, crypto_secretstream_xchacha20poly1305_KEYBYTES>;
using Salt = std::array<Byte, crypto_pwhash_SALTBYTES>;

inline constexpr std::size_t MIN_WEBP_FILE_SIZE = 30;
inline constexpr std::size_t MAX_COVER_IMAGE_FILE_SIZE = 20ULL * 1024 * 1024;
inline constexpr std::size_t MAX_PROGRAM_FILE_SIZE = 1ULL * 1024 * 1024 * 1024;
inline constexpr std::size_t MAX_COVER_IMAGE_PIXELS = 40ULL * 1024 * 1024;
inline constexpr std::size_t MAX_BLUESKY_UPLOAD_SIZE = 1'000'000;

enum class Mode : Byte { conceal, recover };
enum class Option : Byte { None, Bluesky };

enum class FileTypeCheck : Byte {
  cover_image = 1,
  embedded_image = 2,
  data_file = 3
};
