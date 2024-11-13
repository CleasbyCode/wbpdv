const std::string decryptFile(std::vector<uint8_t>&Image_Vec, std::vector<uint8_t>&Decrypted_File_Vec) {

	constexpr uint16_t 
		FILE_SIZE_INDEX 		= 0x345,
		ENCRYPTED_FILENAME_INDEX 	= 0x331,
		EXTRA_BYTE_FLAG_INDEX 		= 0x349;

	constexpr uint8_t
		XOR_KEY_LENGTH 			= 234,
		PIN_LENGTH 			= 4;
	
	const uint32_t EMBEDDED_FILE_SIZE = getByteValue(Image_Vec, FILE_SIZE_INDEX);

	bool isChunkSizeOdd = Image_Vec[EXTRA_BYTE_FLAG_INDEX];

	uint16_t 
		encrypted_file_start_index = isChunkSizeOdd ? 0x353 : 0x352,
		xor_key_index = 0x246,
		decrypt_xor_pos = xor_key_index,
		index_xor_pos = decrypt_xor_pos,
		pin_index = encrypted_file_start_index;

	uint8_t
		encrypted_filename_length = Image_Vec[ENCRYPTED_FILENAME_INDEX - 1],
		xor_key_length = XOR_KEY_LENGTH,
		Xor_Key_Arr[XOR_KEY_LENGTH],
		value_bit_length = 32,
		xor_key_pos = 0,
		char_pos = 0;
		
	const std::string ENCRYPTED_FILENAME { Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX, Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX + encrypted_filename_length };
	
	std::cout << "\nPIN: ";
	uint32_t 
		pin = getPin(),
		encrypted_file_size 	= 0,
		index_pos		= 0;

	valueUpdater(Image_Vec, pin_index, pin, value_bit_length);

	while(xor_key_length--) {
		Image_Vec[decrypt_xor_pos++] = Image_Vec[index_xor_pos++] ^ Image_Vec[pin_index++];
		pin_index = pin_index >= encrypted_file_start_index + PIN_LENGTH ? encrypted_file_start_index : pin_index;
	}
	
	// Read in the xor key stored in the profile data.
	for (int i = 0; XOR_KEY_LENGTH > i; ++i) {
		Xor_Key_Arr[i] = Image_Vec[xor_key_index++]; 
	}

	// Remove profile data & cover image data from vector. Leaving just the encrypted/compressed data file.
	std::vector<uint8_t> Temp_Vec(Image_Vec.begin() + encrypted_file_start_index, Image_Vec.begin() + encrypted_file_start_index + EMBEDDED_FILE_SIZE);
	Image_Vec = std::move(Temp_Vec);

	encrypted_file_size = static_cast<uint32_t>(Image_Vec.size());

	std::string decrypted_filename;

	while (encrypted_filename_length--) {
		decrypted_filename += ENCRYPTED_FILENAME[char_pos++] ^ Xor_Key_Arr[xor_key_pos++];
	}
			
	while (encrypted_file_size > index_pos) {
		Decrypted_File_Vec.emplace_back(Image_Vec[index_pos++] ^ Xor_Key_Arr[xor_key_pos++ % XOR_KEY_LENGTH]);
	}
	std::vector<uint8_t>().swap(Image_Vec);
	return decrypted_filename;
}
