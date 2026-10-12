#pragma once

#include "common.h"

#include <span>

void zlibDeflate(vBytes &data_vec,
                 std::size_t max_output_size = MAX_PROGRAM_FILE_SIZE);
// Inflates a single zlib stream directly into `fd` (capped at
// MAX_PROGRAM_FILE_SIZE) and returns the number of bytes written.
[[nodiscard]] std::size_t zlibInflateToFd(std::span<const Byte> compressed,
                                          int fd);
