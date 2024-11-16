//	WEBP Data Vehicle (wbpout v1.1) Created by Nicholas Cleasby (@CleasbyCode) 13/11/2024
//
//	To compile program (Linux):
// 	$ g++ main.cpp -O2 -lz -s -o wbpout
//	$ sudo cp wbpout /usr/bin

// 	Run it:
// 	$ wbpout

#include "wbpout.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cout << "\nUsage: wbpout <file_embedded_image>\n\t\bwbpout --info\n\n";
        return 1;
    }

    if (std::string(argv[1]) == "--info") {
        displayInfo();
        return 0;
    }

    const std::string IMAGE_FILENAME = std::string(argv[1]);

    constexpr const char* REG_EXP = ("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");
    const std::regex regex_pattern(REG_EXP);

    if (!std::regex_match(IMAGE_FILENAME, regex_pattern)) {
        std::cerr << "\nInvalid Input Error: Characters not supported by this program found within filename arguments.\n\n";
        return 1;
    }

    const std::filesystem::path IMAGE_PATH(IMAGE_FILENAME);
    const std::string IMAGE_EXTENSION = IMAGE_PATH.extension().string();

    if (IMAGE_EXTENSION != ".webp") {
        std::cerr << "\nFile Type Error: Invalid file extension. Expecting only \"webp\" image extension.\n\n";
        return 1;
    }

    if (!std::filesystem::exists(IMAGE_FILENAME)) {
        std::cerr << "\nImage File Error: File not found. Check the filename and try again.\n\n";
        return 1;
    }
    wbpOut(IMAGE_FILENAME);
}
