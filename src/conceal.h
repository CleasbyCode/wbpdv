#pragma once

#include "common.h"

// Computes and validates the exact standard-mode RIFF size before a potentially
// large output allocation. `iccp_needs_padding` is true when the ICCP payload
// has an odd length and therefore takes a RIFF pad byte. Exposed so boundary
// behavior can be unit-tested with sizes alone.
[[nodiscard]] std::size_t
checkedStandardOutputSize(std::size_t profile_size, bool iccp_needs_padding,
                          std::size_t image_size);

void concealData(vBytes &image_vec, Option option,
                 const fs::path &data_file_path);
