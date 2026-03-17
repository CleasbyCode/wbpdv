// WebP Data Vehicle (wbpdv v3.1). Created by Nicholas Cleasby (@CleasbyCode) 13/11/2024

#include "args.h"
#include "conceal.h"
#include "io_utils.h"
#include "recover.h"

#include <iostream>
#include <print>
#include <stdexcept>

int main(int argc, char** argv) {
	try {
		if (sodium_init() < 0) {
			throw std::runtime_error("Libsodium initialization failed!");
		}

		auto args_opt = ProgramArgs::parse(argc, argv);
		if (!args_opt) return 0;

		auto& args = *args_opt;

		vBytes image_vec = readFile(
			args.image_file_path,
			args.mode == Mode::conceal ? FileTypeCheck::cover_image : FileTypeCheck::embedded_image
		);

		if (args.mode == Mode::conceal) {
			concealData(image_vec, args.option, args.data_file_path);
		} else {
			recoverData(image_vec);
		}
	}
	catch (const std::exception& e) {
		std::println(std::cerr, "\n{}\n", e.what());
		return 1;
	}
	return 0;
}
