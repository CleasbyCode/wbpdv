#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sodium.h>
#include <string>
#include <vector>
namespace fs = std::filesystem;
using Byte = std::uint8_t;
using vBytes = std::vector<Byte>;
using Key = std::array<Byte, crypto_secretstream_xchacha20poly1305_KEYBYTES>;
using Salt = std::array<Byte, crypto_pwhash_SALTBYTES>;
inline constexpr std::size_t MIN_WEBP_FILE_SIZE = 30;
inline constexpr std::size_t MAX_COVER_IMAGE_FILE_SIZE = 20ULL * 1024 * 1024;
inline constexpr std::size_t MAX_PROGRAM_FILE_SIZE = 1ULL * 1024 * 1024 * 1024;
inline constexpr std::size_t MAX_COVER_IMAGE_PIXELS = 40ULL * 1024 * 1024;
inline constexpr std::size_t MAX_BLUESKY_UPLOAD_SIZE = 1'000'000;
enum class Mode : Byte { conceal, recover };
enum class Option : Byte { None, Bluesky };
enum class FileTypeCheck : Byte { cover_image = 1, embedded_image = 2, data_file = 3 };
