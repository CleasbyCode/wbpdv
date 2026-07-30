#pragma once

#include "common.h"

// Computes and validates the exact standard-mode RIFF size before a potentially
// large output allocation. Exposed so boundary behavior can be unit-tested with
// sizes alone.
[[nodiscard]] std::size_t
checkedStandardOutputSize(std::size_t profile_size,
                          std::size_t iccp_data_size,
                          std::size_t image_size);

void concealData(vBytes &image_vec, Option option,
                 const fs::path &data_file_path);
