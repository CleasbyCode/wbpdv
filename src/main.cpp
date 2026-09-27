#include "args.h"
#include "conceal.h"
#include "io_utils.h"
#include "recover.h"
#include <iostream>
#include <print>
#include <stdexcept>

namespace {

[[nodiscard]] FileTypeCheck imageTypeForMode(Mode mode) {
  return mode == Mode::conceal ? FileTypeCheck::cover_image
                               : FileTypeCheck::embedded_image;
}

void runCommand(const ProgramArgs &args) {
  vBytes image_vec =
      readFile(args.image_file_path, imageTypeForMode(args.mode));
  if (args.mode == Mode::conceal) {
    concealData(image_vec, args.option, args.data_file_path);
    return;
  }

  recoverData(image_vec);
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (sodium_init() < 0) {
      throw std::runtime_error("Libsodium initialization failed!");
    }

    if (auto args = ProgramArgs::parse(argc, argv)) {
      runCommand(*args);
    }
  } catch (const std::exception &e) {
    std::println(std::cerr, "\n{}\n", e.what());
    return 1;
  }

  return 0;
}
