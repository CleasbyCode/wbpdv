#pragma once

#include "common.h"

#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

[[nodiscard]] bool hasValidFilename(const fs::path &p);
// Returns nullptr when p.filename() is usable as an embedded/recovered
// filename, otherwise a static description of the first rule it breaks, for
// callers that report why a name was rejected. Applies hasValidFilename's
// character rules plus the reserved-name rules: rejects ".", "..", a leading
// '.' or '-', and a trailing space or '.'.
[[nodiscard]] const char *embeddedFilenameProblem(const fs::path &p);
// hasValidFilename plus the reserved-name rules used for embedded/recovered
// filenames: rejects ".", "..", a leading '.' or '-', and a trailing space or '.'.
[[nodiscard]] bool hasSafeEmbeddedFilename(const fs::path &p);
[[nodiscard]] bool
hasFileExtension(const fs::path &p,
                 std::initializer_list<std::string_view> exts);
[[nodiscard]] std::size_t
getFileSizeChecked(const fs::path &path,
                   FileTypeCheck file_type = FileTypeCheck::data_file);
[[nodiscard]] vBytes
readFile(const fs::path &path,
         FileTypeCheck file_type = FileTypeCheck::data_file);

[[nodiscard]] inline std::span<Byte> byteSpan(vBytes &data) noexcept {
  return {data.data(), data.size()};
}

[[nodiscard]] inline std::span<const Byte>
byteSpan(const vBytes &data) noexcept {
  return {data.data(), data.size()};
}

[[nodiscard]] inline bool spanHasRange(std::span<const Byte> data,
                                       std::size_t index, std::size_t length) {
  return index <= data.size() && length <= (data.size() - index);
}

[[nodiscard]] inline std::size_t
checkedAddSize(std::size_t lhs, std::size_t rhs, std::string_view message) {
  if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
    throw std::runtime_error(std::string(message));
  }
  return lhs + rhs;
}

[[nodiscard]] inline std::size_t
checkedMulSize(std::size_t lhs, std::size_t rhs, std::string_view message) {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    throw std::runtime_error(std::string(message));
  }
  return lhs * rhs;
}

// Overflow-checked sum of three or more terms, so callers stop threading the
// same error message through a chain of checkedAddSize calls.
template <class... Rest>
[[nodiscard]] inline std::size_t checkedSum(std::string_view message,
                                            std::size_t first, Rest... rest) {
  std::size_t total = first;
  ((total = checkedAddSize(total, static_cast<std::size_t>(rest), message)),
   ...);
  return total;
}

inline void requireSpanRange(std::span<const Byte> data, std::size_t index,
                             std::size_t length, std::string_view message) {
  if (!spanHasRange(data, index, length)) {
    throw std::runtime_error(std::string(message));
  }
}

[[nodiscard]] inline std::uint16_t readLe16At(
    std::span<const Byte> data, std::size_t offset,
    std::string_view message = "Internal Error: readLe16At out of bounds.") {
  requireSpanRange(data, offset, 2, message);
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1])
                                    << 8);
}

[[nodiscard]] inline std::uint16_t readBe16At(
    std::span<const Byte> data, std::size_t offset,
    std::string_view message = "Internal Error: readBe16At out of bounds.") {
  requireSpanRange(data, offset, 2, message);
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset])
                                    << 8) |
         static_cast<std::uint16_t>(data[offset + 1]);
}

[[nodiscard]] inline std::uint32_t readLe32At(
    std::span<const Byte> data, std::size_t offset,
    std::string_view message = "Internal Error: readLe32At out of bounds.") {
  requireSpanRange(data, offset, 4, message);
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

[[nodiscard]] inline std::uint32_t readBe32At(
    std::span<const Byte> data, std::size_t offset,
    std::string_view message = "Internal Error: readBe32At out of bounds.") {
  requireSpanRange(data, offset, 4, message);
  return (static_cast<std::uint32_t>(data[offset]) << 24) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
         static_cast<std::uint32_t>(data[offset + 3]);
}

inline void writeLe24At(
    vBytes &data, std::size_t offset, std::uint32_t value,
    std::string_view message = "Internal Error: writeLe24At out of bounds.") {
  requireSpanRange(data, offset, 3, message);
  data[offset + 0] = static_cast<Byte>(value & 0xFF);
  data[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
  data[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
}

inline void writeLe32At(
    vBytes &data, std::size_t offset, std::uint32_t value,
    std::string_view message = "Internal Error: writeLe32At out of bounds.") {
  requireSpanRange(data, offset, 4, message);
  data[offset + 0] = static_cast<Byte>(value & 0xFF);
  data[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
  data[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
  data[offset + 3] = static_cast<Byte>((value >> 24) & 0xFF);
}

inline void writeBe32At(
    vBytes &data, std::size_t offset, std::uint32_t value,
    std::string_view message = "Internal Error: writeBe32At out of bounds.") {
  requireSpanRange(data, offset, 4, message);
  data[offset + 0] = static_cast<Byte>((value >> 24) & 0xFF);
  data[offset + 1] = static_cast<Byte>((value >> 16) & 0xFF);
  data[offset + 2] = static_cast<Byte>((value >> 8) & 0xFF);
  data[offset + 3] = static_cast<Byte>(value & 0xFF);
}

// Append `bytes` to `out`, growing it with an overflow-checked resize.
// `message` is the error thrown if the combined size would overflow
// std::size_t.
void appendBytes(vBytes &out, std::span<const Byte> bytes,
                 std::string_view message);

// RAII scrub of a byte buffer's contents. Wipes on destruction unless
// release()'d; wipe() scrubs early and disarms so a later free cannot double
// up. Every buffer holding payload plaintext, compressed plaintext or key
// material owns one, so a throw anywhere in the pipeline cannot release those
// bytes back to the heap unscrubbed.
class SensitiveBytesGuard {
public:
  explicit SensitiveBytesGuard(vBytes &bytes) noexcept : bytes_(&bytes) {}
  ~SensitiveBytesGuard() { wipe(); }

  SensitiveBytesGuard(const SensitiveBytesGuard &) = delete;
  SensitiveBytesGuard &operator=(const SensitiveBytesGuard &) = delete;

  void wipe() noexcept {
    if (bytes_ != nullptr && !bytes_->empty()) {
      sodium_memzero(bytes_->data(), bytes_->size());
    }
    bytes_ = nullptr;
  }

  // Drop ownership without wiping (the bytes are not sensitive, or were moved
  // into a buffer that carries its own guard).
  void release() noexcept { bytes_ = nullptr; }

private:
  vBytes *bytes_{};
};

void closeFdNoThrow(int &fd) noexcept;
void closeFdOrThrow(int &fd);
void writeAllToFd(int fd, std::span<const Byte> data);
void syncFdOrThrow(int fd, std::string_view error_prefix);

// Move-only owner of an O_EXCL-created file. On destruction (unless release()'d):
// closes the fd if still open and best-effort unlinks the path. Call release()
// after a successful finalize (keep the file) or after renameat2 moves it away.
class UniqueFileHandle {
public:
  UniqueFileHandle() noexcept = default;
  UniqueFileHandle(fs::path path, int file_descriptor) noexcept;
  ~UniqueFileHandle();

  UniqueFileHandle(const UniqueFileHandle &) = delete;
  UniqueFileHandle &operator=(const UniqueFileHandle &) = delete;
  UniqueFileHandle(UniqueFileHandle &&other) noexcept;
  UniqueFileHandle &operator=(UniqueFileHandle &&other) noexcept;

  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

  void closeOrThrow();
  void closeNoThrow() noexcept;
  // Drop ownership without unlinking (file is the final output, or was renamed).
  void release() noexcept;

private:
  void reset() noexcept;

  fs::path path_{};
  int fd_{-1};
};

[[nodiscard]] UniqueFileHandle
createUniqueFile(const fs::path &directory, std::string_view prefix,
                 std::string_view suffix, std::size_t max_attempts,
                 std::string_view create_error_prefix,
                 std::string_view exhausted_error);
// Linux-only: renameat2(RENAME_NOREPLACE). Never clobbers an existing target,
// returning false instead, so callers can retry the staged file under a
// different name without an exists-then-rename race. Other failures throw.
// A successful rename is the commit. A later directory-sync failure is
// reported as a warning; the fully written target remains in place.
[[nodiscard]] bool
tryCommitPathAtomically(const fs::path &staged_path,
                        const fs::path &output_path,
                        std::string_view failure_error_prefix);
// Atomically commits a staged file to a randomly named, non-existing target.
// Collisions are retried and the containing directory is synced on success.
[[nodiscard]] fs::path commitToUniquePathAtomically(
    const fs::path &staged_path, const fs::path &directory,
    std::string_view prefix, std::string_view suffix, std::size_t max_attempts,
    std::string_view failure_error_prefix, std::string_view exhausted_error);
void cleanupPathNoThrow(const fs::path &path) noexcept;
