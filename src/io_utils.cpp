#include "io_utils.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <format>
#include <ranges>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
namespace {

constexpr std::size_t MAX_STREAM_SIZE = static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max());

[[nodiscard]] bool isImageType(FileTypeCheck file_type) {
  return file_type == FileTypeCheck::cover_image || file_type == FileTypeCheck::embedded_image;
}

[[nodiscard]] std::size_t safeFileSize(const fs::path& path) {
  std::error_code ec;
  const std::uintmax_t raw_file_size = fs::file_size(path, ec);
  if (ec) {
    throw std::runtime_error(std::format("Error: Failed to get file size for \"{}\".", path.string()));
  }
  if (raw_file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
      raw_file_size > static_cast<std::uintmax_t>(MAX_STREAM_SIZE)) {
    throw std::runtime_error("Error: File is too large for this build.");
  }
  return static_cast<std::size_t>(raw_file_size);
}

void syncParentDirectory(const fs::path& file_path) {
  fs::path dir = file_path.parent_path();
  if (dir.empty()) {
    dir = ".";
  }

  const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (dir_fd >= 0) {
    ::fsync(dir_fd);
    ::close(dir_fd);
  }
}
} // namespace

bool hasValidFilename(const fs::path& p) {
  const std::string fn = p.filename().string();
  return !fn.empty() && std::ranges::all_of(fn, [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '@' || c == '%';
  });
}

bool hasFileExtension(const fs::path& p, std::initializer_list<std::string_view> exts) {
  auto e = p.extension().string();
  std::ranges::transform(e, e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return std::ranges::any_of(exts, [&e](std::string_view ext) { return e == ext; });
}

std::size_t getFileSizeChecked(const fs::path& path, FileTypeCheck file_type) {
  if (!hasValidFilename(path)) {
    throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
  }
  std::error_code ec;
  const bool exists = fs::exists(path, ec);
  const bool regular = exists && !ec && fs::is_regular_file(path, ec);
  if (ec || !exists || !regular) {
    throw std::runtime_error(std::format("Error: File \"{}\" not found or not a regular file.", path.string()));
  }
  const std::size_t file_size = safeFileSize(path);
  if (!file_size)
    throw std::runtime_error("Error: File is empty.");
  if (isImageType(file_type)) {
    if (!hasFileExtension(path, {".webp"}))
      throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".webp\".");
    if (file_type == FileTypeCheck::cover_image) {
      if (MIN_WEBP_FILE_SIZE > file_size)
        throw std::runtime_error("File Error: Invalid image file size.");
      if (file_size > MAX_COVER_IMAGE_FILE_SIZE)
        throw std::runtime_error("Image File Error: Cover image file exceeds maximum size limit.");
    }
  }
  if (file_size > MAX_PROGRAM_FILE_SIZE)
    throw std::runtime_error("Error: File exceeds program size limit.");
  return file_size;
}

vBytes readFile(const fs::path& path, FileTypeCheck file_type) {
  const std::size_t file_size = getFileSizeChecked(path, file_type);
  int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int fd = ::open(path.c_str(), flags);
  if (fd < 0) {
    const std::error_code ec(errno, std::generic_category());
    throw std::runtime_error(std::format("Failed to open file: {} ({})", path.string(), ec.message()));
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::uintmax_t>(st.st_size) != file_size) {
    ::close(fd);
    throw std::runtime_error(std::format("Failed to read file: {} (stat mismatch)", path.string()));
  }
  vBytes vec(file_size);
  std::size_t read_total = 0;
  while (read_total < file_size) {
    const std::size_t remaining = file_size - read_total;
    const std::size_t chunk = std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t rc = ::read(fd, vec.data() + read_total, chunk);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      const std::error_code ec(errno, std::generic_category());
      ::close(fd);
      throw std::runtime_error(std::format("Failed to read file: {} ({})", path.string(), ec.message()));
    }
    if (rc == 0) {
      ::close(fd);
      throw std::runtime_error("Failed to read full file: partial read");
    }
    read_total += static_cast<std::size_t>(rc);
  }
  ::close(fd);
  return vec;
}

void closeFdNoThrow(int& fd) noexcept {
  if (fd < 0)
    return;
  ::close(fd);
  fd = -1;
}

void closeFdOrThrow(int& fd) {
  if (fd < 0)
    return;
  const int rc = ::close(fd);
  const int saved_errno = errno;
  fd = -1;
  if (rc == 0 || saved_errno == EINTR) {
    return;
  }
  const std::error_code ec(saved_errno, std::generic_category());
  throw std::runtime_error(std::format("Write Error: Failed to finalize output file: {}", ec.message()));
}

void writeAllToFd(int fd, std::span<const Byte> data) {
  std::size_t written = 0;
  while (written < data.size()) {
    const std::size_t remaining = data.size() - written;
    const std::size_t chunk_size = std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t rc = ::write(fd, data.data() + static_cast<std::ptrdiff_t>(written), chunk_size);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      const std::error_code ec(errno, std::generic_category());
      throw std::runtime_error(std::format("Write Error: Failed to write complete output file: {}", ec.message()));
    }
    if (rc == 0) {
      throw std::runtime_error("Write Error: Failed to write complete output file.");
    }
    written += static_cast<std::size_t>(rc);
  }
}

void syncFdOrThrow(int fd, std::string_view error_prefix) {
  if (::fdatasync(fd) < 0) {
    const std::error_code ec(errno, std::generic_category());
    throw std::runtime_error(std::format("{}{}", error_prefix, ec.message()));
  }
}

UniqueFileHandle createUniqueFile(const fs::path& directory,
                                  std::string_view prefix,
                                  std::string_view suffix,
                                  std::size_t max_attempts,
                                  std::string_view create_error_prefix,
                                  std::string_view exhausted_error) {
  for (std::size_t i = 0; i < max_attempts; ++i) {
    const fs::path candidate_name = std::format("{}{}{}", prefix, 100000 + randombytes_uniform(900000), suffix);
    const fs::path candidate = directory.empty() ? candidate_name : (directory / candidate_name);
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = ::open(candidate.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
      return UniqueFileHandle{.path = candidate, .fd = fd};
    }
    if (errno == EEXIST) {
      continue;
    }
    const std::error_code ec(errno, std::generic_category());
    throw std::runtime_error(std::format("{}{}", create_error_prefix, ec.message()));
  }
  throw std::runtime_error(std::string(exhausted_error));
}

void commitPathAtomically(const fs::path& staged_path,
                          const fs::path& output_path,
                          std::string_view exists_error,
                          std::string_view failure_error_prefix) {
#ifdef __linux__
  // Prefer renameat2(RENAME_NOREPLACE) when available so we never clobber
  // an existing file between the existence check and final rename.
  const long rename_rc = ::syscall(SYS_renameat2, AT_FDCWD, staged_path.c_str(), AT_FDCWD, output_path.c_str(), RENAME_NOREPLACE);
  if (rename_rc == 0) {
    syncParentDirectory(output_path);
    return;
  }
  if (errno != ENOSYS && errno != EINVAL) {
    if (errno == EEXIST) {
      throw std::runtime_error(std::string(exists_error));
    }
    const std::error_code ec(errno, std::generic_category());
    throw std::runtime_error(std::format("{}{}", failure_error_prefix, ec.message()));
  }
#endif
  std::error_code ec;
  if (fs::exists(output_path, ec)) {
    throw std::runtime_error(std::string(exists_error));
  }
  if (ec) {
    throw std::runtime_error(std::format("{}{}", failure_error_prefix, ec.message()));
  }
  fs::rename(staged_path, output_path, ec);
  if (ec) {
    throw std::runtime_error(std::format("{}{}", failure_error_prefix, ec.message()));
  }
  syncParentDirectory(output_path);
}

void cleanupPathNoThrow(const fs::path& path) noexcept {
  if (path.empty())
    return;
  std::error_code ec;
  fs::remove(path, ec);
}
