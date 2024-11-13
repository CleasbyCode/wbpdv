bool writeFile(std::vector<uint8_t>& Vec) {
	std::random_device rd;
    	std::mt19937 gen(rd());
    	std::uniform_int_distribution<> dist(10000, 99999);  // Five-digit random number

	const std::string IMAGE_FILENAME = "wpdv_" + std::to_string(dist(gen)) + ".webp";

	std::ofstream file_ofs(IMAGE_FILENAME, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		return false;
	}
	
	constexpr uint8_t WEBP_HEADER_LENGTH = 8;

	const uint32_t 
		IMAGE_SIZE = static_cast<uint32_t>(Vec.size()),
		WEBP_IMAGE_SIZE = IMAGE_SIZE - WEBP_HEADER_LENGTH;

	uint8_t 
		value_bit_length = 32,	
		webp_image_size_field_index = 0x07;

	valueUpdater(Vec, webp_image_size_field_index, WEBP_IMAGE_SIZE, value_bit_length, false);

	file_ofs.write(reinterpret_cast<const char*>(Vec.data()), IMAGE_SIZE);
	
	std::vector<uint8_t>().swap(Vec);
	
	std::cout << "\nSaved \"file-embedded\" WEBP image: " << IMAGE_FILENAME << " (" << IMAGE_SIZE << " bytes).\n";
	return true;
}