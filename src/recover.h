#pragma once

#include "common.h"

// Computes and validates the reconstructed Bluesky profile size without
// allocating it. Exposed for inexpensive boundary tests.
[[nodiscard]] std::size_t
checkedBlueskyProfileSize(std::size_t exif_artist_size,
                          std::size_t xmp_overflow_size);

void recoverData(vBytes &image_vec);
