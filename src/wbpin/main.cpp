//	Webp Data Vehicle (wbpin v1.1) Created by Nicholas Cleasby (@CleasbyCode) 12/11/2024
//
//	To compile program (Linux):
// 	$ g++ main.cpp -O2 -lz -lwebp -s -o wbpin
//	$ sudo cp wbpin /usr/bin

// 	Run it:
// 	$ wbpin

#include "wbpin.h"

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--info") {
        displayInfo();
        return 0;
    }
    
    if (argc !=3) {
        std::cout << "\nUsage: wbpin <cover_image> <data_file>\n\t\bwbpin --info\n\n";
        return 1;
    }

    const std::string IMAGE_FILENAME = argv[1];
    std::string data_filename = argv[2];

    constexpr const char* REG_EXP = ("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");
    const std::regex regex_pattern(REG_EXP);

    if (!std::regex_match(IMAGE_FILENAME, regex_pattern) || !std::regex_match(data_filename, regex_pattern)) {
        std::cerr << "\nInvalid Input Error: Characters not supported by this program found within filename arguments.\n\n";
        return 1;
    }

    const std::filesystem::path
        IMAGE_PATH(IMAGE_FILENAME),
        DATA_FILE_PATH(data_filename);

    const std::string
        IMAGE_EXTENSION = IMAGE_PATH.extension().string(),
        DATA_FILE_EXTENSION = DATA_FILE_PATH.extension().string();

    if (IMAGE_EXTENSION != ".webp") {
        std::cerr << "\nFile Type Error: Invalid file extension. Expecting only \"webp\" image extension.\n\n";
        return 1;
    }

    if (!std::filesystem::exists(IMAGE_FILENAME) || !std::filesystem::exists(data_filename) || !std::filesystem::is_regular_file(data_filename)) {
        std::cerr << (!std::filesystem::exists(IMAGE_FILENAME)
            ? "\nImage File Error: File not found."
            : "\nData File Error: File not found or not a regular file.")
            << " Check the filename and try again.\n\n";
        return 1;
    }

    wbpIn(IMAGE_FILENAME, data_filename);
}