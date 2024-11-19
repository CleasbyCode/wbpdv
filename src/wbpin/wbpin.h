#pragma once

#include <algorithm>
#include <filesystem>
#include <random>
#include <cstdint>
#include <fstream>
#include <regex>
#include <iostream>
#include <string>
#include <vector>
#include <iterator>

#include <C:\Users\Nick\source\zlib-1.3.1\zlib.h>
#include <C:\Users\Nick\source\zlib-1.3.1\decode.h>

#include "profilesVec.cpp"
#include "valueUpdater.cpp"
#include "crc32.cpp"
#include "writeFile.cpp"
#include "searchFunc.cpp"
#include "getByteValue.cpp"
#include "encryptFile.cpp"
#include "deflateFile.cpp"
#include "information.cpp"
#include "wbpin.cpp"

template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>&, uint32_t, const uint8_t, const uint8_t (&)[N]);

uint32_t 
	crcUpdate(uint8_t*, uint32_t),
	encryptFile(std::vector<uint8_t>&, std::vector<uint8_t>&, uint32_t, std::string&),
	deflateFile(std::vector<uint8_t>),
	getByteValue(const std::vector<uint8_t>&, const uint32_t);

bool writeFile(std::vector<uint8_t>&);

void
	valueUpdater(std::vector<uint8_t>&, uint32_t, const uint32_t, uint8_t),
	displayInfo();

int wbpIn(const std::string&, std::string&);