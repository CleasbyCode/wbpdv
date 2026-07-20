// Module-level unit tests for wbpdv (no main.cpp).
// Build/run via: bash tests/run_unit_tests.sh

#include "base64.h"
#include "compression.h"
#include "conceal.h"
#include "encryption.h"
#include "io_utils.h"
#include "recover.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_passes = 0;

void expect(bool cond, std::string_view name) {
  if (cond) {
    ++g_passes;
    std::println("[PASS] {}", name);
    return;
  }
  ++g_failures;
  std::println(std::cerr, "[FAIL] {}", name);
}

void expect_throws(std::string_view name, auto &&fn) {
  try {
    fn();
    ++g_failures;
    std::println(std::cerr, "[FAIL] {} (expected throw)", name);
  } catch (const std::exception &) {
    ++g_passes;
    std::println("[PASS] {}", name);
  } catch (...) {
    ++g_failures;
    std::println(std::cerr, "[FAIL] {} (non-std exception)", name);
  }
}

// --- endian / span helpers -------------------------------------------------

void test_endian_and_span() {
  vBytes buf = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  expect(spanHasRange(buf, 0, 7), "spanHasRange full");
  expect(spanHasRange(buf, 3, 4), "spanHasRange tail");
  expect(!spanHasRange(buf, 3, 5), "spanHasRange overflow");
  expect(!spanHasRange(buf, 8, 0), "spanHasRange index past end");
  expect(spanHasRange(buf, 7, 0), "spanHasRange empty at end");

  expect(readLe16At(buf, 0) == 0x0201, "readLe16At");
  expect(readBe16At(buf, 0) == 0x0102, "readBe16At");
  expect(readLe32At(buf, 0) == 0x04030201u, "readLe32At");
  expect(readBe32At(buf, 0) == 0x01020304u, "readBe32At");

  vBytes out(7, 0);
  writeLe24At(out, 0, 0xAABBCC);
  expect(out[0] == 0xCC && out[1] == 0xBB && out[2] == 0xAA, "writeLe24At");
  writeLe32At(out, 0, 0x11223344);
  expect(readLe32At(out, 0) == 0x11223344u, "writeLe32At/readLe32At roundtrip");
  writeBe32At(out, 0, 0x11223344);
  expect(readBe32At(out, 0) == 0x11223344u, "writeBe32At/readBe32At roundtrip");

  expect(checkedAddSize(10, 20, "x") == 30, "checkedAddSize ok");
  expect_throws("checkedAddSize overflow", [] {
    (void)checkedAddSize(std::numeric_limits<std::size_t>::max(), 1,
                         "overflow");
  });
  expect(checkedMulSize(10, 20, "x") == 200, "checkedMulSize ok");
  expect_throws("checkedMulSize overflow", [] {
    (void)checkedMulSize(std::numeric_limits<std::size_t>::max(), 2,
                         "overflow");
  });
  expect_throws("requireSpanRange oob", [] {
    const Byte b[2] = {1, 2};
    requireSpanRange(std::span<const Byte>(b, 2), 1, 2, "oob");
  });
  expect_throws("readLe16At oob", [] {
    const Byte b[1] = {1};
    (void)readLe16At(std::span<const Byte>(b, 1), 0);
  });
}

// --- filename policy -------------------------------------------------------

void test_filename_policy() {
  // hasValidFilename / hasSafeEmbeddedFilename inspect path.filename() only;
  // full path traversal is rejected later in recover's safeRecoveryPath.
  expect(hasValidFilename("payload.txt"), "valid simple name");
  expect(hasValidFilename("my file.txt"), "valid name with space");
  expect(!hasValidFilename(""), "reject empty");
  expect(!hasValidFilename("a\\b"), "reject backslash in basename");
  expect(!hasValidFilename("a:b"), "reject colon");
  expect(!hasValidFilename("a*b"), "reject star");
  expect(!hasValidFilename("a?b"), "reject question mark");
  expect(hasValidFilename("dir/payload.txt"),
         "path input uses basename only (dir/payload.txt -> payload.txt)");

  expect(hasSafeEmbeddedFilename("payload.txt"), "safe embedded");
  expect(!hasSafeEmbeddedFilename(".hidden"), "reject leading dot");
  expect(!hasSafeEmbeddedFilename("-flag"), "reject leading dash");
  expect(!hasSafeEmbeddedFilename("."), "reject dot");
  expect(!hasSafeEmbeddedFilename(".."), "reject dotdot");
  expect(!hasSafeEmbeddedFilename("name."), "reject trailing dot");
  expect(!hasSafeEmbeddedFilename("name "), "reject trailing space");
  // basename of "../x" is "x", which is otherwise safe; path structure is
  // filtered at recovery time, not here.
  expect(hasSafeEmbeddedFilename("../x"),
         "basename-only: ../x -> x is safe at this layer");
}

// --- base64 ----------------------------------------------------------------

void test_base64() {
  auto roundtrip = [](std::span<const Byte> in, std::string_view name) {
    vBytes encoded;
    binaryToBase64(in, encoded);
    vBytes decoded;
    appendBase64AsBinary(encoded, decoded);
    const bool ok =
        decoded.size() == in.size() &&
        (in.empty() || std::memcmp(decoded.data(), in.data(), in.size()) == 0);
    expect(ok, name);
  };

  {
    vBytes empty_enc;
    binaryToBase64({}, empty_enc);
    expect(empty_enc.empty(), "base64 empty produces no output");
  }

  const Byte one[] = {0x00};
  roundtrip(one, "base64 1 byte");
  const Byte two[] = {0x00, 0xFF};
  roundtrip(two, "base64 2 bytes");
  const Byte three[] = {'M', 'a', 'n'};
  roundtrip(three, "base64 3 bytes (classic Man)");
  std::array<Byte, 64> block{};
  for (std::size_t i = 0; i < block.size(); ++i) {
    block[i] = static_cast<Byte>(i * 3);
  }
  roundtrip(block, "base64 64 bytes");

  // Known vector: "Man" -> "TWFu"
  {
    vBytes enc;
    binaryToBase64(three, enc);
    expect(enc.size() == 4 && enc[0] == 'T' && enc[1] == 'W' && enc[2] == 'F' &&
               enc[3] == 'u',
           "base64 known Man->TWFu");
  }

  expect_throws("base64 decode non-multiple-of-4", [] {
    const Byte bad[] = {'T', 'W', 'F'};
    vBytes out;
    appendBase64AsBinary(bad, out);
  });
  expect_throws("base64 decode invalid alphabet", [] {
    const Byte bad[] = {'T', 'W', 'F', '!'};
    vBytes out;
    appendBase64AsBinary(bad, out);
  });
  expect_throws("base64 decode empty rejected", [] {
    vBytes out;
    appendBase64AsBinary({}, out);
  });
  expect_throws("base64 decode size cap", [] {
    const Byte ok[] = {'T', 'W', 'F', 'u'};
    vBytes out;
    appendBase64AsBinary(ok, out, /*max_decoded_append_size=*/2);
  });

  // The input span may be backed by the append destination. Force a compact
  // capacity so ASan catches the former resize-induced dangling span.
  {
    vBytes aliased = {'M', 'a', 'n'};
    aliased.shrink_to_fit();
    const std::span<const Byte> input(aliased.data(), aliased.size());
    binaryToBase64(input, aliased);
    const vBytes expected = {'M', 'a', 'n', 'T', 'W', 'F', 'u'};
    expect(aliased == expected, "base64 encode aliased append");
  }
  {
    vBytes aliased = {'T', 'W', 'F', 'u'};
    aliased.shrink_to_fit();
    const std::span<const Byte> input(aliased.data(), aliased.size());
    appendBase64AsBinary(input, aliased);
    const vBytes expected = {'T', 'W', 'F', 'u', 'M', 'a', 'n'};
    expect(aliased == expected, "base64 decode aliased append");
  }
  {
    vBytes aliased = {'T', 'W', 'F', '!'};
    aliased.shrink_to_fit();
    const vBytes original = aliased;
    expect_throws("base64 invalid aliased decode rejected", [&] {
      appendBase64AsBinary(
          std::span<const Byte>(aliased.data(), aliased.size()), aliased);
    });
    expect(aliased == original, "base64 invalid alias rolls back append");
  }
}

// --- compression ----------------------------------------------------------

void test_compression() {
  // The compressed stream fits the cap even though libdeflate's worst-case
  // bound is far larger. This exercises the capped destination fast path.
  vBytes original(1U * 1024 * 1024, 0);
  vBytes compressed = original;
  zlibDeflate(compressed, 2048);
  expect(compressed.size() <= 2048,
         "zlib compressible input fits small destination cap");
  zlibInflate(compressed);
  expect(compressed == original, "zlib capped compression roundtrip");

  vBytes stream = {'o', 'n', 'e'};
  zlibDeflate(stream);
  {
    vBytes trailing = stream;
    trailing.push_back(0x00);
    expect_throws("zlib rejects trailing byte", [&] { zlibInflate(trailing); });
  }
  {
    vBytes concatenated = stream;
    concatenated.insert(concatenated.end(), stream.begin(), stream.end());
    expect_throws("zlib rejects concatenated streams",
                  [&] { zlibInflate(concatenated); });
  }
}

// --- final-size accounting ------------------------------------------------

void test_output_size_boundaries() {
  expect(checkedStandardOutputSize(MAX_PROGRAM_FILE_SIZE - 100,
                                   /*iccp_data_size=*/200,
                                   /*image_size=*/100) ==
             MAX_PROGRAM_FILE_SIZE,
         "standard final size accepts exact maximum with even ICCP");
  expect(checkedStandardOutputSize(/*profile_size=*/10,
                                   /*iccp_data_size=*/3,
                                   /*image_size=*/20) == 31,
         "standard final size counts odd ICCP padding");
  expect_throws("standard final size rejects maximum plus one", [] {
    (void)checkedStandardOutputSize(MAX_PROGRAM_FILE_SIZE,
                                    /*iccp_data_size=*/2,
                                    /*image_size=*/1);
  });
  expect_throws("standard final size rejects checked-add overflow", [] {
    (void)checkedStandardOutputSize(std::numeric_limits<std::size_t>::max(),
                                    /*iccp_data_size=*/1,
                                    /*image_size=*/0);
  });

  expect(checkedBlueskyProfileSize(MAX_BLUESKY_UPLOAD_SIZE - 1, 1) ==
             MAX_BLUESKY_UPLOAD_SIZE,
         "Bluesky reconstructed profile accepts exact upload maximum");
  expect_throws("Bluesky reconstructed profile rejects maximum plus one", [] {
    (void)checkedBlueskyProfileSize(MAX_BLUESKY_UPLOAD_SIZE, 1);
  });
  expect_throws("Bluesky reconstructed profile rejects checked-add overflow",
                [] {
                  (void)checkedBlueskyProfileSize(
                      std::numeric_limits<std::size_t>::max(), 1);
                });
}

// --- secretstream via encrypt/decrypt public API ---------------------------

void test_secretstream_roundtrips() {
  auto roundtrip = [](vBytes body, std::string_view filename,
                      std::string_view name) {
    vBytes profile(WEBP_PROFILE_TEMPLATE.begin(), WEBP_PROFILE_TEMPLATE.end());
    const std::uint64_t pin =
        encryptDataFile(profile, body, std::string(filename), WEBP_EMBED_OFFSETS);
    // encrypt clears body
    expect(body.empty(), std::string(name) + " body wiped");

    auto decrypted = decryptDataFileWithPin(profile, WEBP_EMBED_OFFSETS, pin);
    expect(decrypted.has_value(), std::string(name) + " decrypt ok");
    if (decrypted) {
      expect(*decrypted == filename, std::string(name) + " filename");
    }
  };

  // Empty payload body (prefix-only stream).
  roundtrip({}, "a.txt", "secretstream empty body");

  // Small single-chunk body.
  {
    vBytes body = {1, 2, 3, 4, 5};
    vBytes profile(WEBP_PROFILE_TEMPLATE.begin(), WEBP_PROFILE_TEMPLATE.end());
    const auto pin =
        encryptDataFile(profile, body, "small.bin", WEBP_EMBED_OFFSETS);
    auto name = decryptDataFileWithPin(profile, WEBP_EMBED_OFFSETS, pin);
    expect(name && *name == "small.bin", "secretstream small name");
    expect(profile.size() == 5 && profile[0] == 1 && profile[4] == 5,
           "secretstream small plaintext");
  }

  // Multi-chunk: STREAM_CHUNK_SIZE is 1 MiB; use >1 MiB incompressible body.
  {
    constexpr std::size_t kSize = (1u * 1024 * 1024) + 64;
    vBytes body(kSize);
    randombytes_buf(body.data(), body.size());
    vBytes expected = body;
    vBytes profile(WEBP_PROFILE_TEMPLATE.begin(), WEBP_PROFILE_TEMPLATE.end());
    const auto pin =
        encryptDataFile(profile, body, "big.bin", WEBP_EMBED_OFFSETS);
    auto name = decryptDataFileWithPin(profile, WEBP_EMBED_OFFSETS, pin);
    expect(name && *name == "big.bin", "secretstream multi-chunk name");
    expect(profile.size() == expected.size() &&
               std::memcmp(profile.data(), expected.data(), expected.size()) ==
                   0,
           "secretstream multi-chunk plaintext");
  }

  // Wrong PIN fails closed.
  {
    vBytes body = {9, 8, 7};
    vBytes profile(WEBP_PROFILE_TEMPLATE.begin(), WEBP_PROFILE_TEMPLATE.end());
    const auto pin =
        encryptDataFile(profile, body, "x.bin", WEBP_EMBED_OFFSETS);
    const auto wrong = pin == 1 ? 2 : 1;
    auto result =
        decryptDataFileWithPin(profile, WEBP_EMBED_OFFSETS, wrong);
    expect(!result.has_value(), "secretstream wrong pin fails");
  }
}

void test_kdf_metadata_versions() {
  constexpr std::size_t kOpslimitOffset = 48;
  constexpr std::size_t kMemlimitKibOffset = 52;
  constexpr auto kKdf3Magic = std::to_array<Byte>({'K', 'D', 'F', '3'});
  constexpr auto kKdf2Magic = std::to_array<Byte>({'K', 'D', 'F', '2'});

  vBytes body = {4, 3, 2, 1};
  vBytes profile(WEBP_PROFILE_TEMPLATE.begin(), WEBP_PROFILE_TEMPLATE.end());
  const auto pin =
      encryptDataFile(profile, body, "kdf.bin", WEBP_EMBED_OFFSETS);
  const std::size_t metadata = WEBP_EMBED_OFFSETS.kdf_metadata;
  expect(std::memcmp(profile.data() + metadata, kKdf3Magic.data(),
                     kKdf3Magic.size()) == 0,
         "new encryption writes KDF3 metadata");
  expect(profile[metadata + 6] == 1 && profile[metadata + 7] == 0,
         "KDF3 parameter encoding and flags");
  expect(readBe32At(profile, metadata + kOpslimitOffset) == 2,
         "KDF3 stores fixed opslimit");
  expect(readBe32At(profile, metadata + kMemlimitKibOffset) == 64 * 1024,
         "KDF3 stores fixed memory limit");

  {
    vBytes roundtrip = profile;
    auto name = decryptDataFileWithPin(roundtrip, WEBP_EMBED_OFFSETS, pin);
    expect(name && *name == "kdf.bin" &&
               roundtrip == vBytes({4, 3, 2, 1}),
           "KDF3 roundtrip");
  }
  {
    vBytes tampered = profile;
    tampered[metadata + kOpslimitOffset + 3] ^= 1;
    expect_throws("KDF3 rejects tampered opslimit before derivation", [&] {
      (void)decryptDataFileWithPin(tampered, WEBP_EMBED_OFFSETS, pin);
    });
  }
  {
    vBytes tampered = profile;
    tampered[metadata + kMemlimitKibOffset + 3] ^= 1;
    expect_throws("KDF3 rejects tampered memory limit before derivation", [&] {
      (void)decryptDataFileWithPin(tampered, WEBP_EMBED_OFFSETS, pin);
    });
  }
  {
    // KDF2 reserved bytes were random. Re-label a stream derived with the same
    // historical fixed values and fill all reserved bytes with arbitrary data
    // to exercise compatibility with existing golden files.
    vBytes legacy = profile;
    std::memcpy(legacy.data() + metadata, kKdf2Magic.data(), kKdf2Magic.size());
    legacy[metadata + 6] = 0xE1;
    legacy[metadata + 7] = 0x7B;
    for (std::size_t i = kOpslimitOffset; i < KDF_METADATA_REGION_BYTES; ++i) {
      legacy[metadata + i] = static_cast<Byte>(0x80 + i);
    }
    auto name = decryptDataFileWithPin(legacy, WEBP_EMBED_OFFSETS, pin);
    expect(name && *name == "kdf.bin" && legacy == vBytes({4, 3, 2, 1}),
           "KDF2 historical fixed-parameter compatibility");
  }
}

std::optional<std::string> decryptWithPinKeystrokes(
    vBytes encrypted_profile, std::string_view keystrokes) {
  int pipe_fds[2] = {-1, -1};
  if (::pipe(pipe_fds) != 0) {
    throw std::runtime_error("pipe failed");
  }
  try {
    writeAllToFd(
        pipe_fds[1],
        std::span<const Byte>(
            reinterpret_cast<const Byte *>(keystrokes.data()),
            keystrokes.size()));
    closeFdOrThrow(pipe_fds[1]);
  } catch (...) {
    closeFdNoThrow(pipe_fds[0]);
    closeFdNoThrow(pipe_fds[1]);
    throw;
  }

  int saved_stdin = ::dup(STDIN_FILENO);
  if (saved_stdin < 0 || ::dup2(pipe_fds[0], STDIN_FILENO) < 0) {
    closeFdNoThrow(pipe_fds[0]);
    closeFdNoThrow(saved_stdin);
    throw std::runtime_error("stdin redirect failed");
  }
  closeFdNoThrow(pipe_fds[0]);

  auto restore_stdin = [&]() noexcept {
    if (saved_stdin >= 0) {
      (void)::dup2(saved_stdin, STDIN_FILENO);
      closeFdNoThrow(saved_stdin);
    }
  };
  try {
    auto result = decryptDataFile(encrypted_profile, WEBP_EMBED_OFFSETS);
    restore_stdin();
    return result;
  } catch (...) {
    restore_stdin();
    throw;
  }
}

void test_pin_input_editing() {
  vBytes body = {0xA1};
  vBytes profile(WEBP_PROFILE_TEMPLATE.begin(), WEBP_PROFILE_TEMPLATE.end());
  const auto pin =
      encryptDataFile(profile, body, "pin.bin", WEBP_EMBED_OFFSETS);
  const std::string pin_digits = std::to_string(pin);
  std::string padded_pin(20 - pin_digits.size(), '0');
  padded_pin += pin_digits;

  std::string corrected = padded_pin;
  const char actual_last = corrected.back();
  corrected.back() = actual_last == '9' ? '8' : static_cast<char>(actual_last + 1);
  corrected.push_back(static_cast<char>(127));
  corrected.push_back(actual_last);
  corrected.push_back('\n');
  const auto edited = decryptWithPinKeystrokes(profile, corrected);
  expect(edited && *edited == "pin.bin",
         "PIN backspace edits a full 20-digit buffer");

  std::string overlong = padded_pin;
  overlong += "0\n";
  const auto rejected = decryptWithPinKeystrokes(profile, overlong);
  expect(!rejected, "PIN rejects rather than truncates overlong input");
}

// --- EXIF IFD walker via recover public API --------------------------------

// Build a minimal RIFF/WEBP with an EXIF chunk that has the WDV signature
// placed so hasEmbeddedBlueskyPayload accepts it, then varies the TIFF/IFD
// structure to exercise findExifArtistEnd error paths.
vBytes makeBlueskyLikeRiff(std::span<const Byte> exif_payload) {
  // RIFF/WEBP + EXIF chunk (no VP8 needed for recover's chunk walk).
  const std::size_t padded =
      exif_payload.size() + (exif_payload.size() & 1U);
  vBytes out;
  out.reserve(12 + 8 + padded);
  // RIFF header
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  const std::uint32_t riff_size =
      static_cast<std::uint32_t>(4 + 8 + padded); // WEBP + chunk
  out.push_back(static_cast<Byte>(riff_size & 0xFF));
  out.push_back(static_cast<Byte>((riff_size >> 8) & 0xFF));
  out.push_back(static_cast<Byte>((riff_size >> 16) & 0xFF));
  out.push_back(static_cast<Byte>((riff_size >> 24) & 0xFF));
  out.insert(out.end(), {'W', 'E', 'B', 'P'});
  out.insert(out.end(), {'E', 'X', 'I', 'F'});
  const std::uint32_t chunk_size =
      static_cast<std::uint32_t>(exif_payload.size());
  out.push_back(static_cast<Byte>(chunk_size & 0xFF));
  out.push_back(static_cast<Byte>((chunk_size >> 8) & 0xFF));
  out.push_back(static_cast<Byte>((chunk_size >> 16) & 0xFF));
  out.push_back(static_cast<Byte>((chunk_size >> 24) & 0xFF));
  out.insert(out.end(), exif_payload.begin(), exif_payload.end());
  if ((exif_payload.size() & 1U) != 0) {
    out.push_back(0);
  }
  return out;
}

void appendWdvMarker(vBytes &exif) {
  const std::size_t kdf_at = exif.size();
  exif.resize(kdf_at + BLUESKY_WDV_TO_KDF + WDV_SIGNATURE_SPAN, 0);
  std::memcpy(exif.data() + kdf_at + BLUESKY_WDV_TO_KDF, WDV_SIGNATURE.data(),
              WDV_SIGNATURE.size());
}

void test_exif_walker_errors() {
  // Truncated: EXIF too short for TIFF header.
  {
    vBytes exif = {'E', 'x', 'i', 'f', 0, 0, 'M', 'M'};
    // Need WDV + KDF space for bluesky path detection.
    // Without it, recover reports signature check failure (not EXIF corrupt).
    vBytes img = makeBlueskyLikeRiff(exif);
    expect_throws("recover rejects short non-wdv exif",
                  [&] { recoverData(img); });
  }

  // Valid WDV placement but IFD count = 0 -> corrupt EXIF.
  {
    vBytes exif;
    exif.insert(exif.end(), EXIF_HEADER_SIGNATURE.begin(),
                EXIF_HEADER_SIGNATURE.end());
    // TIFF MM * IFD@8
    exif.insert(exif.end(),
                {0x4D, 0x4D, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08});
    // IFD count 0
    exif.push_back(0);
    exif.push_back(0);
    // Pad with KDF+WDV so hasEmbeddedBlueskyPayload is true.
    while (exif.size() < 20) {
      exif.push_back(0);
    }
    appendWdvMarker(exif);
    vBytes img = makeBlueskyLikeRiff(exif);
    expect_throws("recover EXIF IFD count 0", [&] { recoverData(img); });
  }

  // Bad byte order (not MM or II).
  {
    vBytes exif;
    exif.insert(exif.end(), EXIF_HEADER_SIGNATURE.begin(),
                EXIF_HEADER_SIGNATURE.end());
    exif.insert(exif.end(),
                {0x00, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08});
    while (exif.size() < 20) {
      exif.push_back(0);
    }
    appendWdvMarker(exif);
    vBytes img = makeBlueskyLikeRiff(exif);
    expect_throws("recover EXIF bad byte order", [&] { recoverData(img); });
  }

  // Truncated RIFF
  {
    vBytes img = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B'};
    expect_throws("recover truncated riff", [&] { recoverData(img); });
  }

  // Well-formed enough to pass WDV check but missing Artist tag.
  {
    vBytes exif;
    exif.insert(exif.end(), EXIF_HEADER_SIGNATURE.begin(),
                EXIF_HEADER_SIGNATURE.end());
    exif.insert(exif.end(),
                {0x4D, 0x4D, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08});
    // IFD count 1, tag 0x0100 (ImageWidth) not Artist
    exif.push_back(0x00);
    exif.push_back(0x01);
    exif.insert(exif.end(), {0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
                             0x00, 0x01, 0x00, 0x00});
    exif.insert(exif.end(), {0, 0, 0, 0});
    while (exif.size() < 40) {
      exif.push_back(0);
    }
    appendWdvMarker(exif);
    vBytes img = makeBlueskyLikeRiff(exif);
    expect_throws("recover EXIF missing Artist tag",
                  [&] { recoverData(img); });
  }
}

// --- UniqueFileHandle RAII -------------------------------------------------

void test_unique_file_handle_cleanup() {
  const fs::path dir = fs::temp_directory_path();
  fs::path leaked_path;
  {
    auto handle =
        createUniqueFile(dir, "wbpdv_unit_", ".tmp", 64,
                         "create failed: ", "exhausted unique names");
    leaked_path = handle.path();
    expect(fs::exists(leaked_path), "unique file exists after create");
    expect(handle.fd() >= 0, "unique file has open fd");
    // Destroy without release -> should unlink.
  }
  expect(!fs::exists(leaked_path), "unique file unlinked on destroy");

  {
    auto handle =
        createUniqueFile(dir, "wbpdv_unit_", ".tmp", 64,
                         "create failed: ", "exhausted unique names");
    const fs::path kept = handle.path();
    writeAllToFd(handle.fd(), std::span<const Byte>());
    handle.closeOrThrow();
    handle.release();
    expect(fs::exists(kept), "released file kept");
    fs::remove(kept);
  }
}

void test_atomic_fixed_target_commit() {
  fs::path dir;
  for (std::size_t attempt = 0; attempt < 64 && dir.empty(); ++attempt) {
    const fs::path candidate =
        fs::temp_directory_path() /
        std::format("wbpdv_atomic_unit_{}_{}", ::getpid(),
                    randombytes_uniform(1'000'000));
    std::error_code ec;
    if (fs::create_directory(candidate, ec)) {
      dir = candidate;
    } else if (ec && ec != std::errc::file_exists) {
      throw std::runtime_error("unable to create atomic-test directory");
    }
  }
  if (dir.empty()) {
    throw std::runtime_error("unable to allocate atomic-test directory");
  }
  struct DirectoryCleanup {
    fs::path path;
    ~DirectoryCleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{dir};

  auto makeFile = [&](const fs::path &path, std::span<const Byte> bytes) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                    S_IRUSR | S_IWUSR);
    if (fd < 0) {
      throw std::runtime_error("unable to create atomic-test file");
    }
    try {
      if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        throw std::runtime_error("unable to set atomic-test file mode");
      }
      writeAllToFd(fd, bytes);
      closeFdOrThrow(fd);
    } catch (...) {
      closeFdNoThrow(fd);
      throw;
    }
  };

  const fs::path target = dir / "fixed-target.bin";
  const vBytes old_bytes = {'o', 'l', 'd'};
  const vBytes new_bytes = {'n', 'e', 'w'};
  makeFile(target, old_bytes);

  fs::path collided_stage_path;
  {
    auto stage = createUniqueFile(dir, "stage_", ".tmp", 64,
                                  "stage create failed: ",
                                  "stage names exhausted");
    collided_stage_path = stage.path();
    if (::fchmod(stage.fd(), S_IRUSR | S_IWUSR) != 0) {
      throw std::runtime_error("unable to set staged mode");
    }
    writeAllToFd(stage.fd(), new_bytes);
    stage.closeOrThrow();
    expect_throws("atomic commit never replaces existing fixed target", [&] {
      commitPathAtomically(stage.path(), target, "target exists",
                           "atomic commit failed: ");
    });
    expect(fs::exists(stage.path()),
           "atomic collision leaves stage owned for cleanup");
    expect(readFile(target, FileTypeCheck::data_file) == old_bytes,
           "atomic collision preserves target bytes");
  }
  expect(!fs::exists(collided_stage_path),
         "collided stage removed by RAII cleanup");

  fs::remove(target);
  auto stage = createUniqueFile(dir, "stage_", ".tmp", 64,
                                "stage create failed: ",
                                "stage names exhausted");
  const fs::path committed_stage_path = stage.path();
  if (::fchmod(stage.fd(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("unable to set staged mode");
  }
  writeAllToFd(stage.fd(), new_bytes);
  stage.closeOrThrow();
  commitPathAtomically(stage.path(), target, "target exists",
                       "atomic commit failed: ");
  stage.release();

  struct stat status {};
  expect(!fs::exists(committed_stage_path) && fs::exists(target),
         "atomic commit moves stage to fresh target");
  expect(readFile(target, FileTypeCheck::data_file) == new_bytes,
         "atomic commit preserves staged bytes");
  expect(::stat(target.c_str(), &status) == 0 &&
             (status.st_mode & 0777) == (S_IRUSR | S_IWUSR),
         "atomic commit preserves 0600 mode");

  auto unique_stage = createUniqueFile(dir, "stage_", ".tmp", 64,
                                       "stage create failed: ",
                                       "stage names exhausted");
  const fs::path unique_stage_path = unique_stage.path();
  if (::fchmod(unique_stage.fd(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("unable to set staged mode");
  }
  writeAllToFd(unique_stage.fd(), new_bytes);
  unique_stage.closeOrThrow();
  const fs::path unique_target = commitToUniquePathAtomically(
      unique_stage.path(), dir, "result_", ".bin", 64,
      "unique commit failed: ", "unique target names exhausted");
  unique_stage.release();
  struct stat unique_status {};
  expect(unique_target.parent_path() == dir && fs::exists(unique_target),
         "unique atomic commit returns existing target path");
  expect(!fs::exists(unique_stage_path),
         "unique atomic commit removes staged path");
  expect(readFile(unique_target, FileTypeCheck::data_file) == new_bytes,
         "unique atomic commit preserves staged bytes");
  expect(::stat(unique_target.c_str(), &unique_status) == 0 &&
             (unique_status.st_mode & 0777) == (S_IRUSR | S_IWUSR),
         "unique atomic commit preserves 0600 mode");
}

} // namespace

int main() {
  if (sodium_init() < 0) {
    std::println(std::cerr, "sodium_init failed");
    return 2;
  }

  try {
    test_endian_and_span();
    test_filename_policy();
    test_base64();
    test_compression();
    test_output_size_boundaries();
    test_secretstream_roundtrips();
    test_kdf_metadata_versions();
    test_pin_input_editing();
    test_exif_walker_errors();
    test_unique_file_handle_cleanup();
    test_atomic_fixed_target_commit();
  } catch (const std::exception &e) {
    std::println(std::cerr, "Unhandled exception: {}", e.what());
    return 2;
  }

  std::println("\nUnit test summary: PASS={} FAIL={}", g_passes, g_failures);
  return g_failures == 0 ? 0 : 1;
}
