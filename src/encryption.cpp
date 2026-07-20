#include "encryption.h"
#include "io_utils.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <limits>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <termios.h>
#include <unistd.h>
namespace {

constexpr auto KDF_METADATA_MAGIC_V2 =
    std::to_array<Byte>({'K', 'D', 'F', '2'});
constexpr auto KDF_METADATA_MAGIC_V3 =
    std::to_array<Byte>({'K', 'D', 'F', '3'});
constexpr std::size_t KDF_MAGIC_OFFSET = 0;
constexpr std::size_t KDF_ALG_OFFSET = 4;
constexpr std::size_t KDF_SENTINEL_OFFSET = 5;
constexpr std::size_t KDF_PARAMETER_ENCODING_OFFSET = 6;
constexpr std::size_t KDF_FLAGS_OFFSET = 7;
constexpr std::size_t KDF_SALT_OFFSET = 8;
constexpr std::size_t KDF_NONCE_OFFSET = 24;
constexpr std::size_t KDF_OPSLIMIT_OFFSET = 48;
constexpr std::size_t KDF_MEMLIMIT_KIB_OFFSET = 52;
constexpr Byte KDF_ALG_ARGON2ID13 = 1;
constexpr Byte KDF_SENTINEL = 0xA5;
constexpr Byte KDF_PARAMETER_ENCODING_BE32 = 1;
constexpr Byte KDF_FLAGS_NONE = 0;

// KDF2 files were created with libsodium's Argon2id "interactive" preset.
// Record the historical numeric values here rather than consulting the
// mutable convenience macros during recovery.  KDF3 stores and validates the
// same numeric values in its metadata, so future library preset changes cannot
// change the on-disk format.
constexpr unsigned long long KDF_ARGON2ID13_OPSLIMIT = 2;
constexpr std::uint32_t KDF_ARGON2ID13_MEMLIMIT_KIB = 64 * 1024;
constexpr std::size_t KDF_ARGON2ID13_MEMLIMIT =
    static_cast<std::size_t>(KDF_ARGON2ID13_MEMLIMIT_KIB) * 1024;
static_assert(KDF_ARGON2ID13_OPSLIMIT >= crypto_pwhash_OPSLIMIT_MIN);
static_assert(KDF_ARGON2ID13_OPSLIMIT <= crypto_pwhash_OPSLIMIT_MAX);
static_assert(KDF_ARGON2ID13_MEMLIMIT >= crypto_pwhash_MEMLIMIT_MIN);
static_assert(KDF_ARGON2ID13_MEMLIMIT <= crypto_pwhash_MEMLIMIT_MAX);
static_assert(KDF_MEMLIMIT_KIB_OFFSET + sizeof(std::uint32_t) ==
              KDF_METADATA_REGION_BYTES);

constexpr const char *CORRUPT_PROFILE_ERROR =
    "Internal Error: Corrupt profile template.";
constexpr const char *CORRUPT_FILE_ERROR =
    "File Recovery Error: Embedded profile is corrupt.";

constexpr auto PIN_TERMINAL_SIGNALS =
    std::to_array<int>({SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP});
volatile std::sig_atomic_t pending_pin_signal = 0;
volatile std::sig_atomic_t pin_signal_handlers_active = 0;

extern "C" void recordPinSignal(int signal_number) noexcept {
  if (pending_pin_signal == 0) {
    pending_pin_signal = signal_number;
  }
}

struct TermiosGuard {
  termios old{};
  bool active{false};
  sigset_t watched_signals{};
  sigset_t original_signal_mask{};
  std::array<struct sigaction, PIN_TERMINAL_SIGNALS.size()> old_actions{};
  std::size_t installed_actions{0};

  TermiosGuard(const TermiosGuard &) = delete;
  TermiosGuard &operator=(const TermiosGuard &) = delete;

  TermiosGuard() {
    if (!isatty(STDIN_FILENO)) {
      return;
    }
    if (tcgetattr(STDIN_FILENO, &old) != 0) {
      return;
    }
    if (sigemptyset(&watched_signals) != 0) {
      return;
    }
    for (const int signal_number : PIN_TERMINAL_SIGNALS) {
      if (sigaddset(&watched_signals, signal_number) != 0) {
        return;
      }
    }
    if (sigprocmask(SIG_BLOCK, &watched_signals, &original_signal_mask) != 0) {
      return;
    }
    if (pin_signal_handlers_active != 0) {
      (void)sigprocmask(SIG_SETMASK, &original_signal_mask, nullptr);
      return;
    }
    pending_pin_signal = 0;
    struct sigaction action {};
    action.sa_handler = recordPinSignal;
    (void)sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // interrupt read(2), then restore outside the handler
    for (std::size_t i = 0; i < PIN_TERMINAL_SIGNALS.size(); ++i) {
      if (sigaction(PIN_TERMINAL_SIGNALS[i], &action, &old_actions[i]) != 0) {
        restoreActions();
        (void)sigprocmask(SIG_SETMASK, &original_signal_mask, nullptr);
        return;
      }
      ++installed_actions;
    }
    pin_signal_handlers_active = 1;
    termios newt = old;
    const auto mask = static_cast<tcflag_t>(ICANON | ECHO);
    newt.c_lflag &= static_cast<tcflag_t>(~mask);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &newt) == 0) {
      active = true;
    }
    if (!active) {
      pin_signal_handlers_active = 0;
      restoreActions();
    }
    (void)sigprocmask(SIG_SETMASK, &original_signal_mask, nullptr);
  }

  ~TermiosGuard() {
    (void)restore();
  }

  [[nodiscard]] bool maskingEnabled() const noexcept { return active; }

  [[nodiscard]] bool interrupted() const noexcept {
    return active && pending_pin_signal != 0;
  }

  // The signal handler only records the signal, which is async-signal-safe.
  // Restore terminal state and prior dispositions here, then let the caller
  // re-raise the signal with its original semantics.
  [[nodiscard]] int restore() noexcept {
    if (!active && installed_actions == 0) {
      return 0;
    }
    sigset_t previous_mask{};
    const bool signals_blocked =
        sigprocmask(SIG_BLOCK, &watched_signals, &previous_mask) == 0;
    if (active) {
      (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
      active = false;
    }
    const int recorded_signal = static_cast<int>(pending_pin_signal);
    pending_pin_signal = 0;
    pin_signal_handlers_active = 0;
    restoreActions();
    if (signals_blocked) {
      (void)sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    }
    return recorded_signal;
  }

private:
  void restoreActions() noexcept {
    while (installed_actions != 0) {
      --installed_actions;
      (void)sigaction(PIN_TERMINAL_SIGNALS[installed_actions],
                      &old_actions[installed_actions], nullptr);
    }
  }
};

struct SensitiveKeyData {
  Key key{};

  ~SensitiveKeyData() {
    sodium_memzero(key.data(), key.size());
  }

  SensitiveKeyData(const SensitiveKeyData &) = delete;
  SensitiveKeyData &operator=(const SensitiveKeyData &) = delete;
  SensitiveKeyData() = default;
};

struct SensitiveBytesGuard {
  vBytes *bytes{};
  bool active{true};

  SensitiveBytesGuard(const SensitiveBytesGuard &) = delete;
  SensitiveBytesGuard &operator=(const SensitiveBytesGuard &) = delete;

  explicit SensitiveBytesGuard(vBytes &in_bytes) : bytes(&in_bytes) {}

  ~SensitiveBytesGuard() {
    if (active && bytes != nullptr && !bytes->empty()) {
      sodium_memzero(bytes->data(), bytes->size());
    }
  }

  void release() noexcept { active = false; }
};

struct SecretStreamStateGuard {
  crypto_secretstream_xchacha20poly1305_state state{};

  SecretStreamStateGuard(const SecretStreamStateGuard &) = delete;
  SecretStreamStateGuard &operator=(const SecretStreamStateGuard &) = delete;
  SecretStreamStateGuard() = default;

  ~SecretStreamStateGuard() {
    sodium_memzero(&state, sizeof(state));
  }
};

constexpr std::size_t STREAM_CHUNK_SIZE = 1 * 1024 * 1024;
constexpr std::size_t STREAM_FRAME_LEN_BYTES = 4;
using StreamHeader =
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>;
constexpr std::size_t MAX_FILENAME_PREFIX_BYTES =
    1 + static_cast<std::size_t>(std::numeric_limits<Byte>::max());
using FilenamePrefixStorage = std::array<Byte, MAX_FILENAME_PREFIX_BYTES>;

struct FilenamePrefixBuffer {
  FilenamePrefixStorage bytes;
  std::size_t size{0};

  FilenamePrefixBuffer(const FilenamePrefixBuffer &) = delete;
  FilenamePrefixBuffer &operator=(const FilenamePrefixBuffer &) = delete;
  FilenamePrefixBuffer() = default;

  ~FilenamePrefixBuffer() {
    if (size != 0) {
      sodium_memzero(bytes.data(), size);
    }
  }

  [[nodiscard]] std::span<const Byte> span() const noexcept {
    return {bytes.data(), size};
  }
};

[[nodiscard]] std::size_t
computeStreamEncryptedSize(std::size_t plaintext_size) {
  const std::size_t chunks =
      (plaintext_size == 0) ? 1
                            : ((plaintext_size - 1) / STREAM_CHUNK_SIZE) + 1;
  const std::size_t per_chunk_overhead =
      STREAM_FRAME_LEN_BYTES + crypto_secretstream_xchacha20poly1305_ABYTES;
  constexpr std::size_t header_size = StreamHeader{}.size();
  if (plaintext_size > std::numeric_limits<std::size_t>::max() - header_size ||
      chunks > (std::numeric_limits<std::size_t>::max() - header_size -
                plaintext_size) /
                   per_chunk_overhead) {
    throw std::runtime_error(
        "Data File Error: Data file too large to encrypt.");
  }
  return header_size + plaintext_size + (chunks * per_chunk_overhead);
}

void writeFrameLen(vBytes &out, std::uint32_t frame_len) {
  const std::size_t offset = out.size();
  out.resize(offset + STREAM_FRAME_LEN_BYTES);
  writeBe32At(out, offset, frame_len);
}

constexpr std::string_view ENCRYPTED_TOO_LARGE_ERROR =
    "Data File Error: Encrypted data is too large.";

// Encrypt prefix||body into output_vec (appended). On exit, body is zeroed and
// cleared. Writing directly to output_vec eliminates a ciphertext-sized memcpy
// that the caller would otherwise do to splice an intermediate ciphertext
// buffer into its output.
void encryptWithSecretStream(vBytes &output_vec, std::span<const Byte> prefix,
                             vBytes &body, const Key &key,
                             StreamHeader &header) {
  SecretStreamStateGuard stream_state;
  if (crypto_secretstream_xchacha20poly1305_init_push(
          &stream_state.state, header.data(), key.data()) != 0) {
    throw std::runtime_error("crypto_secretstream init_push failed.");
  }
  const std::size_t total =
      checkedAddSize(prefix.size(), body.size(),
                     "Data File Error: Data file too large to encrypt.");
  output_vec.reserve(
      checkedAddSize(output_vec.size(), computeStreamEncryptedSize(total),
                     "Data File Error: Encrypted data is too large."));
  appendBytes(output_vec, std::span<const Byte>(header),
              ENCRYPTED_TOO_LARGE_ERROR);
  vBytes cipher_chunk(STREAM_CHUNK_SIZE +
                      crypto_secretstream_xchacha20poly1305_ABYTES);

  // Only the first chunk ever needs a scratch buffer to join prefix + body;
  // subsequent chunks push straight from body's storage with no extra copies.
  vBytes staging;
  if (!prefix.empty()) {
    staging.reserve(STREAM_CHUNK_SIZE);
  }

  // Store each ciphertext frame as [length][ciphertext] so recovery can
  // validate bounds before attempting to decrypt any chunk data.
  std::size_t logical_offset = 0;
  while (true) {
    const std::size_t remaining = total - logical_offset;
    const std::size_t chunk_len = std::min(remaining, STREAM_CHUNK_SIZE);
    const bool is_final = (logical_offset + chunk_len == total);
    const unsigned char tag =
        is_final ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0;

    const Byte *chunk_ptr = nullptr;
    if (chunk_len != 0) {
      if (logical_offset < prefix.size()) {
        // Chunk straddles prefix and body: build it in a scratch buffer.
        staging.resize(chunk_len);
        const std::size_t from_prefix =
            std::min(chunk_len, prefix.size() - logical_offset);
        std::memcpy(staging.data(), prefix.data() + logical_offset,
                    from_prefix);
        const std::size_t from_body = chunk_len - from_prefix;
        if (from_body > 0) {
          std::memcpy(staging.data() + from_prefix, body.data(), from_body);
        }
        chunk_ptr = staging.data();
      } else {
        chunk_ptr = body.data() + (logical_offset - prefix.size());
      }
    }

    unsigned long long clen_ull = 0;
    if (crypto_secretstream_xchacha20poly1305_push(
            &stream_state.state, cipher_chunk.data(), &clen_ull, chunk_ptr,
            chunk_len, nullptr, 0, tag) != 0) {
      throw std::runtime_error("crypto_secretstream push failed.");
    }
    if (clen_ull > static_cast<unsigned long long>(
                       std::numeric_limits<std::uint32_t>::max())) {
      throw std::runtime_error("crypto_secretstream frame too large.");
    }
    const auto clen = static_cast<std::uint32_t>(clen_ull);
    writeFrameLen(output_vec, clen);
    appendBytes(output_vec, std::span<const Byte>(cipher_chunk.data(), clen),
                ENCRYPTED_TOO_LARGE_ERROR);
    logical_offset += chunk_len;
    if (is_final) {
      break;
    }
  }
  if (!staging.empty()) {
    sodium_memzero(staging.data(), staging.size());
  }
  if (!body.empty()) {
    sodium_memzero(body.data(), body.size());
  }
  body.clear();
}

// Decrypt in place: cipher at storage[cipher_start..cipher_start+cipher_len),
// plaintext written to storage[0..]. Plaintext write trails unread cipher by at
// least (cipher_start + header.size() + 21*N) bytes after frame N — always
// safe. On success, storage is resized to plaintext size. On failure, any
// partial plaintext is wiped.
[[nodiscard]] bool decryptWithSecretStreamInPlace(vBytes &storage,
                                                  std::size_t cipher_start,
                                                  std::size_t cipher_len,
                                                  const Key &key,
                                                  const StreamHeader &header) {
  if (cipher_len < header.size()) {
    return false;
  }
  if (!spanHasRange(storage, cipher_start, cipher_len)) {
    return false;
  }
  if (!std::ranges::equal(
          std::span<const Byte>(storage.data() + cipher_start, header.size()),
          header)) {
    return false;
  }
  SecretStreamStateGuard stream_state;
  if (crypto_secretstream_xchacha20poly1305_init_pull(
          &stream_state.state, header.data(), key.data()) != 0) {
    return false;
  }
  vBytes plain_chunk(STREAM_CHUNK_SIZE);
  SensitiveBytesGuard plain_chunk_guard(plain_chunk);

  std::size_t cipher_pos = cipher_start + header.size();
  const std::size_t cipher_end = cipher_start + cipher_len;
  std::size_t plain_pos = 0;
  bool has_final_tag = false;

  auto fail = [&]() -> bool {
    sodium_memzero(storage.data(), plain_pos);
    return false;
  };

  while (cipher_pos < cipher_end) {
    if (cipher_end - cipher_pos < STREAM_FRAME_LEN_BYTES) {
      return fail();
    }
    const std::uint32_t frame_len = readBe32At(storage, cipher_pos);
    cipher_pos += STREAM_FRAME_LEN_BYTES;
    if (frame_len < crypto_secretstream_xchacha20poly1305_ABYTES ||
        frame_len > cipher_end - cipher_pos) {
      return fail();
    }
    const std::size_t max_plain_chunk =
        static_cast<std::size_t>(frame_len) -
        crypto_secretstream_xchacha20poly1305_ABYTES;
    if (max_plain_chunk > plain_chunk.size()) {
      return fail();
    }
    unsigned long long mlen = 0;
    unsigned char tag = 0;
    if (crypto_secretstream_xchacha20poly1305_pull(
            &stream_state.state, plain_chunk.data(), &mlen, &tag,
            storage.data() + cipher_pos, frame_len, nullptr, 0) != 0) {
      return fail();
    }
    if (mlen > max_plain_chunk) {
      return fail();
    }
    if (mlen > 0) {
      std::memcpy(storage.data() + plain_pos, plain_chunk.data(), mlen);
      plain_pos += mlen;
    }
    cipher_pos += frame_len;
    if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
      has_final_tag = true;
      break;
    }
  }
  if (!has_final_tag || cipher_pos != cipher_end) {
    return fail();
  }
  // Wipe trailing ciphertext bytes before truncating.
  if (plain_pos < storage.size()) {
    sodium_memzero(storage.data() + plain_pos, storage.size() - plain_pos);
  }
  storage.resize(plain_pos);
  return true;
}

struct KdfParameters {
  unsigned long long opslimit;
  std::size_t memlimit;
};

constexpr KdfParameters FIXED_KDF_PARAMETERS = {
    KDF_ARGON2ID13_OPSLIMIT, KDF_ARGON2ID13_MEMLIMIT};

void deriveKeyFromPin(Key &out_key, std::uint64_t pin, const Salt &salt,
                      const KdfParameters &parameters) {
  std::array<char, 32> pin_buf{};
  auto [ptr, ec] =
      std::to_chars(pin_buf.data(), pin_buf.data() + pin_buf.size(), pin);
  if (ec != std::errc{}) {
    sodium_memzero(pin_buf.data(), pin_buf.size());
    throw std::runtime_error("KDF Error: Failed to encode recovery PIN.");
  }
  const auto pin_len = static_cast<unsigned long long>(ptr - pin_buf.data());
  const int rc = crypto_pwhash(
      out_key.data(), out_key.size(), pin_buf.data(), pin_len, salt.data(),
      parameters.opslimit, parameters.memlimit,
      crypto_pwhash_ALG_ARGON2ID13);
  sodium_memzero(pin_buf.data(), pin_buf.size());
  if (rc != 0) {
    throw std::runtime_error("KDF Error: Unable to derive encryption key.");
  }
}

void writeKdfMetadata(vBytes &profile_vec, const ProfileOffsets &offsets,
                      const Salt &salt, const StreamHeader &stream_header) {
  randombytes_buf(profile_vec.data() + offsets.kdf_metadata,
                  KDF_METADATA_REGION_BYTES);
  std::ranges::copy(KDF_METADATA_MAGIC_V3,
                    profile_vec.begin() +
                        static_cast<std::ptrdiff_t>(offsets.kdf_metadata +
                                                    KDF_MAGIC_OFFSET));
  profile_vec[offsets.kdf_metadata + KDF_ALG_OFFSET] = KDF_ALG_ARGON2ID13;
  profile_vec[offsets.kdf_metadata + KDF_SENTINEL_OFFSET] = KDF_SENTINEL;
  profile_vec[offsets.kdf_metadata + KDF_PARAMETER_ENCODING_OFFSET] =
      KDF_PARAMETER_ENCODING_BE32;
  profile_vec[offsets.kdf_metadata + KDF_FLAGS_OFFSET] = KDF_FLAGS_NONE;
  requireSpanRange(profile_vec, offsets.kdf_metadata + KDF_SALT_OFFSET,
                   salt.size(), CORRUPT_PROFILE_ERROR);
  requireSpanRange(profile_vec, offsets.kdf_metadata + KDF_NONCE_OFFSET,
                   stream_header.size(), CORRUPT_PROFILE_ERROR);
  std::ranges::copy(salt, profile_vec.begin() +
                              static_cast<std::ptrdiff_t>(offsets.kdf_metadata +
                                                          KDF_SALT_OFFSET));
  std::ranges::copy(
      stream_header,
      profile_vec.begin() +
          static_cast<std::ptrdiff_t>(offsets.kdf_metadata + KDF_NONCE_OFFSET));
  writeBe32At(profile_vec, offsets.kdf_metadata + KDF_OPSLIMIT_OFFSET,
              static_cast<std::uint32_t>(KDF_ARGON2ID13_OPSLIMIT));
  writeBe32At(profile_vec, offsets.kdf_metadata + KDF_MEMLIMIT_KIB_OFFSET,
              KDF_ARGON2ID13_MEMLIMIT_KIB);
}

void readKdfMetadata(const vBytes &profile_vec, const ProfileOffsets &offsets,
                     Salt &salt, StreamHeader &stream_header) {
  requireSpanRange(profile_vec, offsets.kdf_metadata + KDF_SALT_OFFSET,
                   salt.size(), CORRUPT_FILE_ERROR);
  requireSpanRange(profile_vec, offsets.kdf_metadata + KDF_NONCE_OFFSET,
                   stream_header.size(), CORRUPT_FILE_ERROR);
  std::ranges::copy_n(
      profile_vec.begin() +
          static_cast<std::ptrdiff_t>(offsets.kdf_metadata + KDF_SALT_OFFSET),
      static_cast<std::ptrdiff_t>(salt.size()), salt.begin());
  std::ranges::copy_n(
      profile_vec.begin() +
          static_cast<std::ptrdiff_t>(offsets.kdf_metadata + KDF_NONCE_OFFSET),
      static_cast<std::ptrdiff_t>(stream_header.size()),
      stream_header.begin());
}

[[nodiscard]] bool metadataMagicEquals(std::span<const Byte> data,
                                       std::size_t base_index,
                                       std::span<const Byte> magic) {
  return spanHasRange(data, base_index + KDF_MAGIC_OFFSET, magic.size()) &&
         std::ranges::equal(
             data.subspan(base_index + KDF_MAGIC_OFFSET, magic.size()), magic);
}

[[nodiscard]] KdfParameters
readKdfParameters(std::span<const Byte> data, std::size_t base_index) {
  requireSpanRange(data, base_index, KDF_METADATA_REGION_BYTES,
                   CORRUPT_FILE_ERROR);
  const bool common_fields_valid =
      data[base_index + KDF_ALG_OFFSET] == KDF_ALG_ARGON2ID13 &&
      data[base_index + KDF_SENTINEL_OFFSET] == KDF_SENTINEL;

  if (metadataMagicEquals(data, base_index, KDF_METADATA_MAGIC_V2)) {
    if (!common_fields_valid) {
      throw std::runtime_error(CORRUPT_FILE_ERROR);
    }
    // KDF2's unused bytes were randomized, so only its defined fields can be
    // validated.  Always use the historical literal parameters.
    return FIXED_KDF_PARAMETERS;
  }

  if (!metadataMagicEquals(data, base_index, KDF_METADATA_MAGIC_V3)) {
    throw std::runtime_error(
        "File Decryption Error: Unsupported legacy encrypted file format. "
        "Use an older wbpdv release to recover this file.");
  }
  if (!common_fields_valid ||
      data[base_index + KDF_PARAMETER_ENCODING_OFFSET] !=
          KDF_PARAMETER_ENCODING_BE32 ||
      data[base_index + KDF_FLAGS_OFFSET] != KDF_FLAGS_NONE) {
    throw std::runtime_error(
        "File Decryption Error: Invalid or unsupported KDF3 metadata.");
  }

  const std::uint32_t encoded_opslimit =
      readBe32At(data, base_index + KDF_OPSLIMIT_OFFSET);
  const std::uint32_t encoded_memlimit_kib =
      readBe32At(data, base_index + KDF_MEMLIMIT_KIB_OFFSET);
  if (encoded_opslimit != KDF_ARGON2ID13_OPSLIMIT ||
      encoded_memlimit_kib != KDF_ARGON2ID13_MEMLIMIT_KIB) {
    throw std::runtime_error(
        "File Decryption Error: Invalid or unsupported KDF3 parameters.");
  }
  return FIXED_KDF_PARAMETERS;
}

[[nodiscard]] std::uint64_t generateRecoveryPin() {
  std::uint64_t pin = 0;
  while (pin == 0) {
    randombytes_buf(&pin, sizeof(pin));
  }
  return pin;
}

[[nodiscard]] std::span<const Byte>
makeFilenamePrefix(const std::string &data_filename,
                   FilenamePrefixBuffer &prefix_buffer) {
  if (data_filename.empty() ||
      data_filename.size() >
          static_cast<std::size_t>(std::numeric_limits<Byte>::max())) {
    throw std::runtime_error("Data File Error: Invalid data filename length.");
  }

  prefix_buffer.size = 1 + data_filename.size();
  prefix_buffer.bytes[0] = static_cast<Byte>(data_filename.size());
  std::memcpy(prefix_buffer.bytes.data() + 1, data_filename.data(),
              data_filename.size());
  return prefix_buffer.span();
}

[[nodiscard]] std::string extractFilenamePrefix(vBytes &payload) {
  if (payload.empty() || payload[0] == 0) {
    throw std::runtime_error(CORRUPT_FILE_ERROR);
  }
  const std::size_t filename_len = payload[0];
  const std::size_t prefix_len = 1 + filename_len;
  requireSpanRange(payload, 0, prefix_len, CORRUPT_FILE_ERROR);
  std::string decrypted_filename(
      reinterpret_cast<const char *>(payload.data() + 1), filename_len);

  const std::size_t remaining = payload.size() - prefix_len;
  if (remaining > 0) {
    std::memmove(payload.data(),
                 payload.data() + static_cast<std::ptrdiff_t>(prefix_len),
                 remaining);
  }
  sodium_memzero(payload.data() + static_cast<std::ptrdiff_t>(remaining),
                 prefix_len);
  payload.resize(remaining);
  return decrypted_filename;
}

[[nodiscard]] std::uint64_t getPin() {
  constexpr auto MAX_UINT64_STR = std::string_view{"18446744073709551615"};
  constexpr std::size_t MAX_PIN_LENGTH = 20;
  std::print("\nPIN: ");
  std::fflush(stdout);
  std::array<char, MAX_PIN_LENGTH> input{};
  std::size_t input_len = 0;
  std::size_t excess_digits = 0;
  bool read_failed = false;
  char ch{};
  TermiosGuard termios_guard;
  auto wipe_input = [&]() {
    sodium_memzero(input.data(), input.size());
    input_len = 0;
    excess_digits = 0;
  };
  while (!termios_guard.interrupted()) {
    const ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
    if (bytes_read == 0) {
      break;
    }
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      read_failed = true;
      break;
    }
    if (ch == '\n' || ch == '\r') {
      break;
    }
    if (ch == '\b' || ch == 127) {
      if (excess_digits != 0) {
        --excess_digits;
      } else if (input_len != 0) {
        input[--input_len] = '\0';
      } else {
        continue;
      }
      if (termios_guard.maskingEnabled()) {
        std::print("\b \b");
        std::fflush(stdout);
      }
      continue;
    }
    if (ch >= '0' && ch <= '9') {
      if (input_len < input.size() && excess_digits == 0) {
        input[input_len++] = ch;
      } else if (excess_digits != std::numeric_limits<std::size_t>::max()) {
        ++excess_digits;
      }
      if (termios_guard.maskingEnabled()) {
        std::print("*");
        std::fflush(stdout);
      }
    }
  }
  const int interrupted_signal = termios_guard.restore();
  std::println("");
  std::fflush(stdout);
  if (interrupted_signal != 0) {
    wipe_input();
    (void)::raise(interrupted_signal);
    return 0;
  }
  const std::string_view input_view(input.data(), input_len);
  if (read_failed || excess_digits != 0 || input_view.empty() ||
      (input_view.length() == MAX_PIN_LENGTH && input_view > MAX_UINT64_STR)) {
    wipe_input();
    return 0;
  }
  std::uint64_t result = 0;
  auto [ptr, ec] =
      std::from_chars(input.data(), input.data() + input_len, result);
  if (ec != std::errc{} || ptr != input.data() + input_len) {
    wipe_input();
    return 0;
  }
  wipe_input();
  return result;
}
} // namespace

std::uint64_t encryptDataFile(vBytes &profile_vec, vBytes &data_vec,
                              const std::string &data_filename,
                              const ProfileOffsets &offsets) {
  requireSpanRange(profile_vec, offsets.kdf_metadata, KDF_METADATA_REGION_BYTES,
                   CORRUPT_PROFILE_ERROR);
  if (offsets.encrypted_file != profile_vec.size()) {
    throw std::runtime_error(CORRUPT_PROFILE_ERROR);
  }
  FilenamePrefixBuffer prefix_buffer;
  const std::span<const Byte> prefix =
      makeFilenamePrefix(data_filename, prefix_buffer);
  const std::size_t prefix_len = prefix.size();
  if (data_vec.size() > std::numeric_limits<std::size_t>::max() - prefix_len) {
    throw std::runtime_error(
        "Data File Error: Data file too large to encrypt.");
  }
  SensitiveKeyData key;
  Salt salt{};
  StreamHeader stream_header{};
  const std::uint64_t pin = generateRecoveryPin();
  randombytes_buf(salt.data(), salt.size());
  deriveKeyFromPin(key.key, pin, salt, FIXED_KDF_PARAMETERS);
  encryptWithSecretStream(profile_vec, prefix, data_vec, key.key,
                          stream_header);
  writeKdfMetadata(profile_vec, offsets, salt, stream_header);
  return pin;
}

std::optional<std::string>
decryptDataFileWithPin(vBytes &profile_vec, const ProfileOffsets &offsets,
                       std::uint64_t recovery_pin) {
  requireSpanRange(profile_vec, offsets.kdf_metadata, KDF_METADATA_REGION_BYTES,
                   CORRUPT_FILE_ERROR);
  if (offsets.encrypted_file > profile_vec.size()) {
    throw std::runtime_error(CORRUPT_FILE_ERROR);
  }
  const KdfParameters kdf_parameters =
      readKdfParameters(profile_vec, offsets.kdf_metadata);
  SensitiveKeyData key;
  Salt salt{};
  StreamHeader stream_header{};
  readKdfMetadata(profile_vec, offsets, salt, stream_header);
  deriveKeyFromPin(key.key, recovery_pin, salt, kdf_parameters);
  sodium_memzero(&recovery_pin, sizeof(recovery_pin));
  const std::size_t ciphertext_length =
      profile_vec.size() - offsets.encrypted_file;
  const std::size_t min_stream_cipher_size =
      StreamHeader{}.size() + STREAM_FRAME_LEN_BYTES +
      crypto_secretstream_xchacha20poly1305_ABYTES;
  if (ciphertext_length < min_stream_cipher_size) {
    throw std::runtime_error(CORRUPT_FILE_ERROR);
  }
  if (!decryptWithSecretStreamInPlace(profile_vec, offsets.encrypted_file,
                                      ciphertext_length, key.key,
                                      stream_header)) {
    return std::nullopt;
  }
  return extractFilenamePrefix(profile_vec);
}

std::optional<std::string> decryptDataFile(vBytes &profile_vec,
                                           const ProfileOffsets &offsets) {
  return decryptDataFileWithPin(profile_vec, offsets, getPin());
}
