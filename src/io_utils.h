#pragma once
#include "common.h"
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
[[nodiscard]] bool hasValidFilename(const fs::path &p);
[[nodiscard]] bool hasFileExtension(const fs::path &p, std::initializer_list<std::string_view> exts);
[[nodiscard]] std::size_t getFileSizeChecked(const fs::path &path, FileTypeCheck file_type = FileTypeCheck::data_file);
[[nodiscard]] vBytes readFile(const fs::path &path, FileTypeCheck file_type = FileTypeCheck::data_file);
[[nodiscard]] inline bool spanHasRange(std::span<const Byte> data, std::size_t index, std::size_t length) {
  return index <= data.size() && length <= (data.size() - index);
}
[[nodiscard]] inline std::size_t checkedAddSize(std::size_t lhs, std::size_t rhs, std::string_view message) {
  if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
    throw std::runtime_error(std::string(message));
  }
  return lhs + rhs;
}
[[nodiscard]] inline std::size_t checkedMulSize(std::size_t lhs, std::size_t rhs, std::string_view message) {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    throw std::runtime_error(std::string(message));
  }
  return lhs * rhs;
}
inline void requireSpanRange(std::span<const Byte> data, std::size_t index, std::size_t length, std::string_view message) {
  if (!spanHasRange(data, index, length)) { throw std::runtime_error(std::string(message)); }
}
[[nodiscard]] inline std::uint16_t readLe16At(std::span<const Byte> data, std::size_t offset,
                                              std::string_view message = "Internal Error: readLe16At out of bounds.") {
  requireSpanRange(data, offset, 2, message);
  return static_cast<std::uint16_t>(data[offset]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8);
}
[[nodiscard]] inline std::uint16_t readBe16At(std::span<const Byte> data, std::size_t offset,
                                              std::string_view message = "Internal Error: readBe16At out of bounds.") {
  requireSpanRange(data, offset, 2, message);
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset]) << 8) | static_cast<std::uint16_t>(data[offset + 1]);
}
[[nodiscard]] inline std::uint32_t readLe32At(std::span<const Byte> data, std::size_t offset,
                                              std::string_view message = "Internal Error: readLe32At out of bounds.") {
  requireSpanRange(data, offset, 4, message);
  return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8) | (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}
[[nodiscard]] inline std::uint32_t readBe32At(std::span<const Byte> data, std::size_t offset,
                                              std::string_view message = "Internal Error: readBe32At out of bounds.") {
  requireSpanRange(data, offset, 4, message);
  return (static_cast<std::uint32_t>(data[offset]) << 24) | (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 8) | static_cast<std::uint32_t>(data[offset + 3]);
}
inline void writeLe24At(vBytes &data, std::size_t offset, std::uint32_t value, std::string_view message = "Internal Error: writeLe24At out of bounds.") {
  requireSpanRange(data, offset, 3, message);
  data[offset + 0] = static_cast<Byte>(value & 0xFF);
  data[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
  data[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
}
inline void writeLe32At(vBytes &data, std::size_t offset, std::uint32_t value, std::string_view message = "Internal Error: writeLe32At out of bounds.") {
  requireSpanRange(data, offset, 4, message);
  data[offset + 0] = static_cast<Byte>(value & 0xFF);
  data[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
  data[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
  data[offset + 3] = static_cast<Byte>((value >> 24) & 0xFF);
}
inline void writeBe32At(vBytes &data, std::size_t offset, std::uint32_t value, std::string_view message = "Internal Error: writeBe32At out of bounds.") {
  requireSpanRange(data, offset, 4, message);
  data[offset + 0] = static_cast<Byte>((value >> 24) & 0xFF);
  data[offset + 1] = static_cast<Byte>((value >> 16) & 0xFF);
  data[offset + 2] = static_cast<Byte>((value >> 8) & 0xFF);
  data[offset + 3] = static_cast<Byte>(value & 0xFF);
}
void closeFdNoThrow(int &fd) noexcept;
void closeFdOrThrow(int &fd);
void writeAllToFd(int fd, std::span<const Byte> data);
void syncFdOrThrow(int fd, std::string_view error_prefix);
struct UniqueFileHandle {
  fs::path path{};
  int fd{-1};
};
[[nodiscard]] UniqueFileHandle createUniqueFile(const fs::path &directory, std::string_view prefix, std::string_view suffix, std::size_t max_attempts,
                                                std::string_view create_error_prefix, std::string_view exhausted_error);
void commitPathAtomically(const fs::path &staged_path, const fs::path &output_path, std::string_view exists_error, std::string_view failure_error_prefix);
void cleanupPathNoThrow(const fs::path &path) noexcept;
