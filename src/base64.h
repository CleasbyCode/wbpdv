#pragma once
#include "common.h"
#include <limits>
#include <span>
void binaryToBase64(std::span<const Byte> binary_data, vBytes &output_vec);
void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes &destination_vec,
                          std::size_t max_decoded_append_size = std::numeric_limits<std::size_t>::max());
