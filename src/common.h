// WEBP Data Vehicle (wbpdv v3.1). Created by Nicholas Cleasby (@CleasbyCode) 13/11/2024

#pragma once

#include <sodium.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using Byte   = std::uint8_t;
using vBytes = std::vector<Byte>;

using Key  = std::array<Byte, crypto_secretstream_xchacha20poly1305_KEYBYTES>;
using Salt = std::array<Byte, crypto_pwhash_SALTBYTES>;

enum class Mode : Byte { conceal, recover };
enum class Option : Byte { None, Bluesky };

enum class FileTypeCheck : Byte {
	cover_image    = 1,
	embedded_image = 2,
	data_file      = 3
};
