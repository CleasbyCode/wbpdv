#include "args.h"

#include <format>
#include <print>
#include <stdexcept>
#include <string_view>

namespace {

void displayInfo() {
  std::print(R"(

WEBP Data Vehicle (wbpdv v3.1)
Created by Nicholas Cleasby (@CleasbyCode) 13/11/2024.

wbpdv is a metadata "steganography-like" command-line tool used for concealing and extracting
any file type within and from a WEBP image.

──────────────────────────
Compile & run (Linux)
──────────────────────────

  $ sudo apt-get install libsodium-dev zlib1g-dev libwebp-dev

  $ chmod +x compile_wbpdv.sh
  $ ./compile_wbpdv.sh

  Compilation successful. Executable 'wbpdv' created.

  $ sudo cp wbpdv /usr/bin
  $ wbpdv

──────────────────────────
Usage
──────────────────────────

  wbpdv conceal [-b] <cover_image> <secret_file>
  wbpdv recover <cover_image>
  wbpdv --info

──────────────────────────
Platform compatibility & size limits
──────────────────────────

Share your "file-embedded" WEBP image on the following compatible sites.

Size limit is measured by the combined size of cover image + data file:

	• Mastodon  (16 MB)
	• Tumblr    (9 MB)
	• Bluesky   (1,000,000 Bytes / ~1MB)

Bluesky - Total output image file must not exceed 1,000,000 bytes (use -b option).

	• Cover image + compressed/encrypted data file must fit within 1,000,000 Bytes.
	• Use a smaller cover image to maximize data file capacity for Bluesky.
	• Note: The compressed & encrypted data file (if it exceeds 64KB) is base64-encoded,
	  which increases its size by ~33%. Account for this overhead when estimating capacity.

──────────────────────────
Modes
──────────────────────────

  conceal - Compresses, encrypts and embeds your secret data file within a WEBP cover image.

  recover - Decrypts, uncompresses and extracts the concealed data file from a WEBP cover image
            (recovery PIN required).

──────────────────────────
Platform options for conceal mode
──────────────────────────

-b (Bluesky) : Creates compatible "file-embedded" WEBP images for posting on Bluesky.

$ wbpdv conceal -b my_image.webp hidden.doc

These images are only compatible for posting on Bluesky.

You must use the Python script "bsky_post.py" (in the repo's src folder) to post images to Bluesky.
Posting via the Bluesky website or mobile app will NOT work.

You also need to create an app password for your Bluesky account to use with the script:-
https://bsky.app/settings/app-passwords

Here is a basic usage examples for the bsky_post.py Python script:-

Standard image post to your profile/account.

$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx
--image your_image.webp --alt-text "alt-text here [optional]" "standard post text here [required]"

)");
}

} // namespace

void ProgramArgs::die(const std::string &usage) {
  throw std::runtime_error(usage);
}

namespace {

[[nodiscard]] std::string programName(int argc, char **argv) {
  if (argc >= 1 && argv != nullptr && argv[0] != nullptr) {
    return fs::path(argv[0]).filename().string();
  }
  return "wbpdv";
}

[[nodiscard]] std::string buildUsage(std::string_view prog) {
  return std::format("Usage: {} conceal [-b] <cover_image> <secret_file>\n     "
                     "  {} recover <cover_image>\n       {} --info",
                     prog, prog, prog);
}

[[nodiscard]] const char *requireArg(int argc, char **argv, int index,
                                     const std::string &usage) {
  if (argv == nullptr || index < 0 || index >= argc || argv[index] == nullptr) {
    ProgramArgs::die(usage);
  }
  return argv[index];
}

[[nodiscard]] ProgramArgs parseConcealArgs(int argc, char **argv,
                                           const std::string &usage) {
  ProgramArgs out{Mode::conceal};
  int i = 2;

  if (std::string_view(requireArg(argc, argv, i, usage)) == "-b") {
    out.option = Option::Bluesky;
    ++i;
  }

  if (i + 2 != argc) {
    ProgramArgs::die(usage);
  }

  out.image_file_path = requireArg(argc, argv, i, usage);
  out.data_file_path = requireArg(argc, argv, i + 1, usage);
  return out;
}

[[nodiscard]] ProgramArgs parseRecoverArgs(int argc, char **argv,
                                           const std::string &usage) {
  if (argc != 3) {
    ProgramArgs::die(usage);
  }
  return ProgramArgs{Mode::recover, Option::None,
                     requireArg(argc, argv, 2, usage), ""};
}

} // namespace

std::optional<ProgramArgs> ProgramArgs::parse(int argc, char **argv) {
  const std::string usage = buildUsage(programName(argc, argv));

  if (argc < 2 || argv == nullptr) {
    die(usage);
  }

  const std::string_view command = requireArg(argc, argv, 1, usage);
  if (command == "--info") {
    displayInfo();
    return std::nullopt;
  }

  if (command == "conceal") {
    return parseConcealArgs(argc, argv, usage);
  }

  if (command == "recover") {
    return parseRecoverArgs(argc, argv, usage);
  }

  die(usage);
}
