#pragma once

#include "common.h"

// Computes and validates the reconstructed Bluesky profile size without
// allocating it. Exposed for inexpensive boundary tests.
[[nodiscard]] std::size_t
checkedBlueskyProfileSize(std::size_t exif_artist_size,
                          std::size_t xmp_overflow_size);

// Builds the `index`th alternative name used when a recovered filename is
// already taken, keeping any multi-part extension intact
// ("archive.tar.gz" -> "archive_1.tar.gz"). Exposed for naming tests.
[[nodiscard]] fs::path recoveryCollisionCandidate(const fs::path &candidate,
                                                  std::size_t index);

void recoverData(vBytes &image_vec);
