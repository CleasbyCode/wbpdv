#pragma once
#include "common.h"
struct ImageInfo {
  int width;
  int height;
};
[[nodiscard]] ImageInfo validateAndPrepareImage(vBytes &image_vec);
