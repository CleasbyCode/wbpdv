#pragma once
#include "common.h"
void zlibDeflate(vBytes &data_vec, std::size_t max_output_size = MAX_PROGRAM_FILE_SIZE);
void zlibInflate(vBytes &data_vec);
