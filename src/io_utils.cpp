#include "io_utils.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <linux/fs.h>
#include <optional>
#include <print>
#include <ranges>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <utility>
namespace {

struct ScopedFd {
  int fd{-1};

  explicit ScopedFd(int file_descriptor) noexcept : fd(file_descriptor) {}
  ~ScopedFd() { closeNoThrow(); }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  [[nodiscard]] int get() const noexcept { return fd; }
  void closeNoThrow() noexcept { closeFdNoThrow(fd); }
};

[[nodiscard]] bool isImageType(FileTypeCheck file_type) {
  return file_type == FileTypeCheck::cover_image ||
         file_type == FileTypeCheck::embedded_image;
}

// Best-effort durability of the new directory entry. Returns an empty string
// on success, or a reason if the directory could not be opened or synced.
[[nodiscard]] std::string trySyncCurrentDirectory() {
  int opened_fd = -1;
  do {
    opened_fd = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (opened_fd < 0 && errno == EINTR);
  ScopedFd dir_fd(opened_fd);
  if (dir_fd.get() < 0) {
    const std::error_code ec(errno, std::generic_category());
    return std::format("failed to open output directory for sync: {}",
                       ec.message());
  }
  int sync_rc = -1;
  do {
    sync_rc = ::fsync(dir_fd.get());
  } while (sync_rc != 0 && errno == EINTR);
  if (sync_rc != 0) {
    const std::error_code ec(errno, std::generic_category());
    return std::format("failed to sync output directory: {}", ec.message());
  }
  return {};
}

// Six-digit random name. O_EXCL / link / RENAME_NOREPLACE is what makes the
// name safe; the randomness only keeps collisions rare.
[[nodiscard]] fs::path randomName(std::string_view prefix,
                                  std::string_view suffix) {
  return std::format("{}{}{}", prefix, 100000 + randombytes_uniform(900000),
                     suffix);
}

constexpr const char *CORRUPT_RIFF_ERROR =
    "Image File Error: Invalid or corrupt WEBP container.";
constexpr auto RIFF_TAG = std::to_array<Byte>({'R', 'I', 'F', 'F'});
constexpr auto WEBP_TAG = std::to_array<Byte>({'W', 'E', 'B', 'P'});

// Blacklist: path separators, the Windows-reserved set, control bytes and DEL.
// Everything else (spaces, '+', '#', non-ASCII bytes >= 0x80, ...) is allowed,
// matching the jdvrif family so the implementations accept/reject the same
// embedded filenames and stay mutually recoverable.
[[nodiscard]] bool isValidFilenameChar(unsigned char c) {
  switch (c) {
  case '/':
  case '\\':
  case ':':
  case '*':
  case '?':
  case '"':
  case '<':
  case '>':
  case '|':
    return false;
  default:
    return c >= 0x20 && c != 0x7F;
  }
}

// Decodes the UTF-8 sequence at fn[i], returning its code point and advancing
// i past it, or nullopt for a malformed, overlong or surrogate sequence.
[[nodiscard]] std::optional<char32_t> decodeUtf8(std::string_view fn,
                                                 std::size_t &i) {
  const auto lead = static_cast<unsigned char>(fn[i]);
  std::size_t length = 0;
  char32_t cp = 0;
  if (lead < 0x80) {
    ++i;
    return lead;
  }
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    cp = lead & 0x1FU;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    cp = lead & 0x0FU;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    cp = lead & 0x07U;
  } else {
    return std::nullopt;
  }
  if (fn.size() - i < length) {
    return std::nullopt;
  }
  for (std::size_t k = 1; k < length; ++k) {
    const auto cont = static_cast<unsigned char>(fn[i + k]);
    if ((cont & 0xC0U) != 0x80U) {
      return std::nullopt;
    }
    cp = (cp << 6) | (cont & 0x3FU);
  }
  constexpr std::array<char32_t, 5> MIN_FOR_LENGTH{0, 0, 0x80, 0x800, 0x10000};
  if (cp < MIN_FOR_LENGTH[length] || cp > 0x10FFFF ||
      (cp >= 0xD800 && cp <= 0xDFFF)) {
    return std::nullopt;
  }
  i += length;
  return cp;
}

// C1 controls can be interpreted by terminals (U+009B is CSI), and bidi
// controls can visually reorder a name ("invoice<RLO>fdp.exe" displays as
// "invoiceexe.pdf"), so neither may appear in an embedded filename.
[[nodiscard]] bool isDisallowedCodePoint(char32_t cp) {
  return (cp >= 0x80 && cp <= 0x9F) || cp == 0x061C || cp == 0x200E ||
         cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
         (cp >= 0x2066 && cp <= 0x2069);
}

} // namespace

bool hasValidFilename(const fs::path &p) {
  const std::string fn = p.filename().string();
  return !fn.empty() && std::ranges::all_of(fn, isValidFilenameChar);
}

const char *embeddedFilenameProblem(const fs::path &p) {
  const std::string fn = p.filename().string();
  if (fn.empty()) {
    return "the name is empty";
  }
  if (!std::ranges::all_of(fn, isValidFilenameChar)) {
    return "it contains one of / \\ : * ? \" < > | or a control character";
  }
  for (std::size_t i = 0; i < fn.size();) {
    const auto cp = decodeUtf8(fn, i);
    if (!cp) {
      return "it is not valid UTF-8";
    }
    if (isDisallowedCodePoint(*cp)) {
      return "it contains a Unicode control or bidirectional-override "
             "character";
    }
  }
  if (fn == "." || fn == "..") {
    return "\".\" and \"..\" are reserved names";
  }
  if (fn.front() == '.') {
    return "it begins with '.'";
  }
  if (fn.front() == '-') {
    return "it begins with '-'";
  }
  if (fn.back() == ' ') {
    return "it ends with a space";
  }
  if (fn.back() == '.') {
    return "it ends with '.'";
  }
  return nullptr;
}

bool hasSafeEmbeddedFilename(const fs::path &p) {
  return embeddedFilenameProblem(p) == nullptr;
}

bool hasFileExtension(const fs::path &p,
                      std::initializer_list<std::string_view> exts) {
  auto e = p.extension().string();
  std::ranges::transform(e, e.begin(), [](unsigned char c) static {
    return static_cast<char>(std::tolower(c));
  });
  return std::ranges::any_of(exts,
                             [&e](std::string_view ext) { return e == ext; });
}

void appendBytes(vBytes &out, std::span<const Byte> bytes,
                 std::string_view message) {
  if (bytes.empty()) {
    return;
  }
  const std::size_t base = out.size();
  const std::size_t final_size = checkedAddSize(base, bytes.size(), message);
  out.resize(final_size);
  std::memcpy(out.data() + static_cast<std::ptrdiff_t>(base), bytes.data(),
              bytes.size());
}

vBytes readFile(const fs::path &path, FileTypeCheck file_type) {
  if (!hasValidFilename(path)) {
    throw std::runtime_error(
        "Invalid Input Error: Unsupported characters in filename arguments.");
  }
  // Every check below runs on the opened fd, so nothing can swap the file
  // between a check and the read. Deliberately no O_NOFOLLOW: the user named
  // this path, so a symlink to their cover image or payload is the intended
  // target (output files, which this tool creates, do keep O_NOFOLLOW).
  // O_NONBLOCK keeps a FIFO from blocking the open (fstat then rejects it; it
  // has no effect on regular-file reads) and O_NOCTTY keeps a named terminal
  // device from becoming our controlling terminal.
  ScopedFd fd(::open(path.c_str(),
                     O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY));
  struct stat st{};
  if (fd.get() < 0 && errno != ENOENT && errno != ENOTDIR) {
    const std::error_code ec(errno, std::generic_category());
    throw std::runtime_error(std::format("Failed to open file: {} ({})",
                                         path.string(), ec.message()));
  }
  if (fd.get() < 0 || ::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
    throw std::runtime_error(std::format(
        "Error: File \"{}\" not found or not a regular file.", path.string()));
  }
  const auto file_size = static_cast<std::size_t>(st.st_size);
  if (file_size == 0) {
    throw std::runtime_error("Error: File is empty.");
  }
  if (isImageType(file_type)) {
    if (!hasFileExtension(path, {".webp"})) {
      throw std::runtime_error("File Type Error: Invalid image extension. Only "
                               "expecting \".webp\".");
    }
    if (file_type == FileTypeCheck::cover_image) {
      if (MIN_WEBP_FILE_SIZE > file_size) {
        throw std::runtime_error("File Error: Invalid image file size.");
      }
      if (file_size > MAX_COVER_IMAGE_FILE_SIZE) {
        throw std::runtime_error(
            "Image File Error: Cover image file exceeds maximum size limit.");
      }
    }
  }
  if (file_size > MAX_PROGRAM_FILE_SIZE) {
    throw std::runtime_error("Error: File exceeds program size limit.");
  }
  vBytes vec(file_size);
  // Wipe on every exit except a successful return so a failed payload read
  // does not leave plaintext residue on the heap.
  SensitiveBytesGuard wipe_guard(vec);

  // read(2) that retries EINTR and throws on error; returns 0 only at EOF.
  auto read_some = [&](Byte *dest, std::size_t count) {
    while (true) {
      const ssize_t rc = ::read(fd.get(), dest, count);
      if (rc >= 0) {
        return static_cast<std::size_t>(rc);
      }
      if (errno != EINTR) {
        const std::error_code ec(errno, std::generic_category());
        throw std::runtime_error(std::format("Failed to read file: {} ({})",
                                             path.string(), ec.message()));
      }
    }
  };
  // Then probe one byte further, so a file that grew while being read is
  // rejected as surely as one that shrank.
  std::size_t read_total = 0;
  Byte trailing{};
  while (read_total < file_size) {
    const std::size_t rc =
        read_some(vec.data() + read_total,
                  std::min<std::size_t>(file_size - read_total, 1uz << 30));
    if (rc == 0) {
      break;
    }
    read_total += rc;
  }
  if (read_total != file_size || read_some(&trailing, 1) != 0) {
    throw std::runtime_error(
        std::format("Failed to read file: {} (file changed while reading)",
                    path.string()));
  }
  fd.closeNoThrow();
  wipe_guard.release();
  return vec;
}

void closeFdNoThrow(int &fd) noexcept {
  if (fd < 0) {
    return;
  }
  ::close(fd);
  fd = -1;
}

void closeFdOrThrow(int &fd) {
  if (fd < 0) {
    return;
  }
  const int rc = ::close(fd);
  const int saved_errno = errno;
  fd = -1;
  if (rc == 0 || saved_errno == EINTR) {
    return;
  }
  const std::error_code ec(saved_errno, std::generic_category());
  throw std::runtime_error(std::format(
      "Write Error: Failed to finalize output file: {}", ec.message()));
}

void writeAllToFd(int fd, std::span<const Byte> data) {
  std::size_t written = 0;
  while (written < data.size()) {
    const std::size_t remaining = data.size() - written;
    const std::size_t chunk_size = std::min<std::size_t>(
        remaining,
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t rc = ::write(
        fd, data.data() + static_cast<std::ptrdiff_t>(written), chunk_size);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::error_code ec(errno, std::generic_category());
      throw std::runtime_error(
          std::format("Write Error: Failed to write complete output file: {}",
                      ec.message()));
    }
    if (rc == 0) {
      throw std::runtime_error(
          "Write Error: Failed to write complete output file.");
    }
    written += static_cast<std::size_t>(rc);
  }
}

void syncFdOrThrow(int fd, std::string_view error_prefix) {
  int rc = -1;
  do {
    rc = ::fdatasync(fd);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    const std::error_code ec(errno, std::generic_category());
    throw std::runtime_error(std::format("{}{}", error_prefix, ec.message()));
  }
}

OutputFile OutputFile::create(mode_t mode) {
  const int fd = ::open(".", O_TMPFILE | O_WRONLY | O_CLOEXEC, mode);
  if (fd >= 0) {
    return OutputFile(fd, {});
  }
  // EOPNOTSUPP: this filesystem has no O_TMPFILE. EISDIR: a pre-3.11 kernel
  // that does not know the flag and saw only its O_DIRECTORY bit.
  if (errno != EOPNOTSUPP && errno != EISDIR) {
    const std::error_code ec(errno, std::generic_category());
    throw std::runtime_error(std::format(
        "Write Error: Unable to create output file: {}", ec.message()));
  }
  constexpr std::size_t MAX_ATTEMPTS = 1024;
  for (std::size_t i = 0; i < MAX_ATTEMPTS; ++i) {
    fs::path candidate = randomName(".wbpdv_tmp_", "");
    const int staged_fd =
        ::open(candidate.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
    if (staged_fd >= 0) {
      return OutputFile(staged_fd, std::move(candidate));
    }
    if (errno != EEXIST) {
      const std::error_code ec(errno, std::generic_category());
      throw std::runtime_error(std::format(
          "Write Error: Unable to create output file: {}", ec.message()));
    }
  }
  throw std::runtime_error(
      "Write Error: Unable to allocate a temporary output filename.");
}

OutputFile::OutputFile(OutputFile &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      staged_path_(std::exchange(other.staged_path_, {})),
      published_(std::exchange(other.published_, true)) {}

OutputFile::~OutputFile() {
  closeFdNoThrow(fd_);
  if (!published_) {
    cleanupPathNoThrow(staged_path_);
  }
}

bool OutputFile::publish(const fs::path &name) {
  int rc = 0;
  if (staged_path_.empty()) {
    // Give the unnamed inode its first name. link() never replaces an entry,
    // so this is as race-free as RENAME_NOREPLACE. The /proc route needs no
    // privilege; AT_EMPTY_PATH covers a system without /proc mounted.
    const std::string proc_path = std::format("/proc/self/fd/{}", fd_);
    rc = ::linkat(AT_FDCWD, proc_path.c_str(), AT_FDCWD, name.c_str(),
                  AT_SYMLINK_FOLLOW);
    if (rc != 0 && errno == ENOENT) {
      rc = ::linkat(fd_, "", AT_FDCWD, name.c_str(), AT_EMPTY_PATH);
    }
  } else {
    rc = static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD,
                                    staged_path_.c_str(), AT_FDCWD,
                                    name.c_str(), RENAME_NOREPLACE));
  }
  if (rc != 0) {
    if (errno == EEXIST) {
      return false;
    }
    const std::error_code ec(errno, std::generic_category());
    throw std::runtime_error(std::format(
        "Write Error: Failed to publish output file: {}", ec.message()));
  }
  published_ = true;
  // The data was synced before publishing and the new name is the commit, so
  // nothing after this point may undo it: report a failure to close or to
  // sync the directory entry as a warning, never as an error.
  closeFdNoThrow(fd_);
  if (const std::string sync_error = trySyncCurrentDirectory();
      !sync_error.empty()) {
    std::println(std::cerr, "Warning: Output file was committed, but {}.",
                 sync_error);
  }
  return true;
}

fs::path OutputFile::publishUnique(std::string_view prefix,
                                   std::string_view suffix) {
  constexpr std::size_t MAX_ATTEMPTS = 2048;
  for (std::size_t i = 0; i < MAX_ATTEMPTS; ++i) {
    fs::path candidate = randomName(prefix, suffix);
    if (publish(candidate)) {
      return candidate;
    }
  }
  throw std::runtime_error("Write Error: Unable to allocate output filename.");
}

std::span<const Byte> parseRiffWebP(std::span<const Byte> file) {
  requireSpanRange(file, 0, 12, CORRUPT_RIFF_ERROR);
  if (!std::ranges::equal(file.first(4), RIFF_TAG) ||
      !std::ranges::equal(file.subspan(8, 4), WEBP_TAG)) {
    throw std::runtime_error("Error: Not a valid WEBP image.");
  }
  const std::size_t riff_total_size =
      8 + static_cast<std::size_t>(readLe32At(file, 4));
  if (riff_total_size < 12 || !spanHasRange(file, 0, riff_total_size)) {
    throw std::runtime_error(CORRUPT_RIFF_ERROR);
  }
  return file.first(riff_total_size);
}

std::vector<RiffChunk> riffChunks(std::span<const Byte> riff_file) {
  std::vector<RiffChunk> chunks;
  for (std::size_t offset = 12; offset < riff_file.size();) {
    requireSpanRange(riff_file, offset, 8, CORRUPT_RIFF_ERROR);
    // 64-bit size_t: offset + 8 + a u32 size + 1 cannot overflow.
    const std::size_t data_size = readLe32At(riff_file, offset + 4);
    const std::size_t data_offset = offset + 8;
    std::size_t padded_size = data_size + (data_size & 1U);
    if (!spanHasRange(riff_file, data_offset, padded_size)) {
      // Some muxers omit the pad byte after an odd-sized final chunk. Accept
      // exactly that case; any other overrun is corruption.
      if (padded_size == data_size ||
          data_offset + data_size != riff_file.size()) {
        throw std::runtime_error(CORRUPT_RIFF_ERROR);
      }
      padded_size = data_size;
    }
    chunks.push_back({riff_file.subspan(offset, 4),
                      riff_file.subspan(data_offset, data_size),
                      riff_file.subspan(offset, 8 + padded_size)});
    offset = data_offset + padded_size;
  }
  return chunks;
}

void cleanupPathNoThrow(const fs::path &path) noexcept {
  if (path.empty()) {
    return;
  }
  std::error_code ec;
  fs::remove(path, ec);
}
