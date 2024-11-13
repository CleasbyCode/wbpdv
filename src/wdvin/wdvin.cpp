int wdvIn(const std::string& IMAGE_FILENAME, std::string& data_filename) {
	
	constexpr uint32_t COMBINED_MAX_FILE_SIZE = 9 * 1024 * 1024;  // 9MB. (image + data file)
	
	const size_t 
		IMAGE_FILE_SIZE 	= std::filesystem::file_size(IMAGE_FILENAME),
		DATA_FILE_SIZE 		= std::filesystem::file_size(data_filename),
		COMBINED_FILE_SIZE 	= DATA_FILE_SIZE + IMAGE_FILE_SIZE;
	
	if (COMBINED_FILE_SIZE > COMBINED_MAX_FILE_SIZE || DATA_FILE_SIZE == 0) {     
   		std::cerr << "\nFile Size Error: " << (DATA_FILE_SIZE == 0 
                 	? "Data file is empty" 
                  	: "Combined size of image and data file exceeds program maximum limit of 9MB")
             	<< ".\n\n";
    		return 1;
	}
	
	std::ifstream
		image_file_ifs(IMAGE_FILENAME, std::ios::binary),
		data_file_ifs(data_filename, std::ios::binary);

	if (!image_file_ifs || !data_file_ifs) {
		std::cerr << "\nRead File Error: Unable to read " << (!image_file_ifs 
			? "image file" 
			: "data file") << ".\n\n";
		return 1;
	}

	std::vector<uint8_t> Image_Vec;
	Image_Vec.resize(IMAGE_FILE_SIZE); 
	
	image_file_ifs.read(reinterpret_cast<char*>(Image_Vec.data()), IMAGE_FILE_SIZE);
	image_file_ifs.close();

	int width = 0, height = 0;

	 // By attempting to retrive the image dimensions, we are also checking for a valid Webp image at the same time.
    	if (!WebPGetInfo(Image_Vec.data(), Image_Vec.size(), &width, &height)) {
       	std::cerr << "Error: Not a valid Webp image." << std::endl;
        	return 1;
    	}

	constexpr uint8_t 
		WEBP_EXTENDED_INDEX = 0x0F,
		WEBP_HEADER_LENGTH = 12;
		
	if (Image_Vec[WEBP_EXTENDED_INDEX] == 'X') {
		constexpr uint8_t 
			WEBP_LL_SIG[] 	{ 0x56, 0x50, 0x38, 0x4C},
			WEBP_LY_SIG[]	{ 0x56, 0x50, 0x38, 0x20},
			WEBP_AN_SIG[]	{ 0x41, 0x4E, 0x49, 0x4D},
			EXIF_SIG[]	{ 0x45, 0x58, 0x49, 0x46},
			XMP_SIG[]	{ 0x58, 0x4D, 0x50, 0x20};

		const uint32_t 
			WEBP_AN_SIG_INDEX = searchFunc(Image_Vec, 0, 0, WEBP_AN_SIG),
			WEBP_LL_SIG_INDEX = searchFunc(Image_Vec, 0, 0, WEBP_LL_SIG),
			WEBP_LY_SIG_INDEX = searchFunc(Image_Vec, 0, 0, WEBP_LY_SIG),	
			XMP_SIG_INDEX = searchFunc(Image_Vec, 0, 0, XMP_SIG),
			WEBP_CHUNK_POS  = std::min(WEBP_LL_SIG_INDEX, WEBP_LY_SIG_INDEX);

		if (WEBP_AN_SIG_INDEX != Image_Vec.size()) {
		 	std::cerr << "\nImage File Error: Webp animation image files not supported.\n\n";
			return 1;
		}

		// Erase chunks in the order of last to first.
		if (XMP_SIG_INDEX != Image_Vec.size()) {	
			Image_Vec.erase(Image_Vec.begin() + XMP_SIG_INDEX, Image_Vec.end());
		}
		
		const uint32_t EXIF_SIG_INDEX = searchFunc(Image_Vec, 0, 0, EXIF_SIG);

		if (EXIF_SIG_INDEX != Image_Vec.size()) {
			Image_Vec.erase(Image_Vec.begin() + EXIF_SIG_INDEX, Image_Vec.end());
		}
		
		// Erase x bytes from start of image file until start of image chunk, removes chunks like ICCP...
		Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + WEBP_CHUNK_POS);
		
	} else {
		// Erase 12 byte header.
	 	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + WEBP_HEADER_LENGTH);
	}

	// Update image dimensions in our profile header.
	uint8_t 
		width_index = 0x19,
		height_index = 0x1C,
		value_bit_length = 16;

	valueUpdater(Profile_Vec, width_index, width - 1, value_bit_length, false);
	valueUpdater(Profile_Vec, height_index, height - 1, value_bit_length, false);

	std::filesystem::path filePath(data_filename);
    	data_filename = filePath.filename().string();

	constexpr uint8_t DATA_FILENAME_MAX_LENGTH = 20;

	const uint8_t DATA_FILENAME_LENGTH = static_cast<uint8_t>(data_filename.length());

	if (DATA_FILENAME_LENGTH > DATA_FILENAME_MAX_LENGTH) {
    		std::cerr << "\nData File Error: Length of data filename is too long.\n\nFor compatibility requirements, length of data filename must not exceed 20 characters.\n\n";
    	 	return 1;
	}

	constexpr uint16_t DATA_FILENAME_LENGTH_INDEX = 0x35A;

	Profile_Vec[DATA_FILENAME_LENGTH_INDEX] = DATA_FILENAME_LENGTH;

	std::vector<uint8_t> File_Vec;
	File_Vec.resize(DATA_FILE_SIZE); 

	data_file_ifs.read(reinterpret_cast<char*>(File_Vec.data()), DATA_FILE_SIZE);
	data_file_ifs.close();

	std::reverse(File_Vec.begin(), File_Vec.end());
	
	uint32_t file_vec_size = deflateFile(File_Vec);
	
	if (File_Vec.empty()) {
		std::cerr << "\nFile Size Error: File is zero bytes. Probable deflate failure.\n\n";
		return 1;
	}
	
	const uint32_t PIN = encryptFile(Profile_Vec, File_Vec, file_vec_size, data_filename);

	std::vector<uint8_t>().swap(File_Vec);
	
	Image_Vec.reserve(Image_Vec.size() + Profile_Vec.size());	

	Image_Vec.insert(Image_Vec.begin(), Profile_Vec.begin(), Profile_Vec.end());

	std::vector<uint8_t>().swap(Profile_Vec);

	if (!writeFile(Image_Vec)) {
		return 1;
	}
	
	std::cout << "\nRecovery PIN: [***" << PIN << "***]\n\nImportant: Please remember to keep your PIN safe, so that you can extract the hidden file.\n";
	std::cout << "\nComplete!\n\n";	

	return 0;
}