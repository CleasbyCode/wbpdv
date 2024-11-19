uint32_t encryptFile(std::vector<uint8_t>& Profile_Vec, std::vector<uint8_t>& File_Vec, uint32_t data_file_size, std::string& data_filename) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
	
	constexpr uint8_t 
		DEFAULT_PIN_INDEX = 0x95,
		DEFAULT_PIN_XOR_INDEX = 0x90,
		XOR_KEY_LENGTH = 234,
		PIN_LENGTH = 9;

	uint16_t 
		profile_data_size = 854,
		deflate_file_size_index = 0x36F,
		data_filename_index = 0x35B,
		xor_key_index = 0x270,
		encrypt_xor_pos = xor_key_index,
		index_xor_pos = encrypt_xor_pos;

	uint32_t
		iccp_chunksize = data_file_size + profile_data_size,
		index_pos = 0;

	uint8_t 
		iccp_chunk_size_field_index = 0x25,
		profile_size_field_index = 0x26,
		data_filename_length = Profile_Vec[data_filename_index - 1],
		xor_key[XOR_KEY_LENGTH],
		pin_index =  DEFAULT_PIN_INDEX,
		pin_xor_index = DEFAULT_PIN_XOR_INDEX,
		value_bit_length = 32,
		xor_key_length = XOR_KEY_LENGTH,
		xor_key_pos = 0,
		char_pos = 0;		

	// Chunk size for WEBP needs to be even. 
	bool isChunkSizeOdd = (iccp_chunksize % 2 != 0);

	if (isChunkSizeOdd) {
		// Insert an extra byte in to the (profile) chunk to make it even.
		// Update some variables to reflect this increase.
		// Set flag in profile to indicate chunk size was odd and an extra byte was added.	
		constexpr uint16_t 
			EXTRA_BYTE_FLAG_INDEX = 0x373,
			DATA_FILE_START_INDEX = 0x37C;

		Profile_Vec.insert(Profile_Vec.begin() + DATA_FILE_START_INDEX, 0x19); 
		++iccp_chunksize;
		Profile_Vec[EXTRA_BYTE_FLAG_INDEX] = true;
	}
	
	// Write chunk size to the ICCP chunk size field
	valueUpdater(Profile_Vec, iccp_chunk_size_field_index, iccp_chunksize, value_bit_length, false);

	// Write chunk size to the color profile size field	
	valueUpdater(Profile_Vec, profile_size_field_index, iccp_chunksize, value_bit_length, true);

	for (int i = 0; i < XOR_KEY_LENGTH; ++i) {
        	xor_key[i] = static_cast<uint8_t>(dis(gen));
		Profile_Vec[xor_key_index++] = xor_key[i];
    	}

	while (data_filename_length--) {
		Profile_Vec[data_filename_index++] = data_filename[char_pos++] ^ xor_key[xor_key_pos++];
	}	
	
	// write deflate file size to our profile
	valueUpdater(Profile_Vec, deflate_file_size_index, data_file_size, value_bit_length, true);

	Profile_Vec.reserve(Profile_Vec.size() + data_file_size);

	while (data_file_size--) {
		Profile_Vec.emplace_back(File_Vec[index_pos++] ^ xor_key[xor_key_pos++ % XOR_KEY_LENGTH]);
	}
	
	xor_key_index = 0x270;

	const uint32_t PIN = crcUpdate(&Profile_Vec[xor_key_index], XOR_KEY_LENGTH);
	valueUpdater(Profile_Vec, pin_index, PIN, value_bit_length, true);

	while(xor_key_length--) {
		Profile_Vec[encrypt_xor_pos++] = Profile_Vec[index_xor_pos++] ^ Profile_Vec[pin_xor_index++];
		pin_xor_index = pin_xor_index >= PIN_LENGTH + DEFAULT_PIN_XOR_INDEX ? DEFAULT_PIN_XOR_INDEX : pin_xor_index;
	}
	
	valueUpdater(Profile_Vec, pin_index, 0, value_bit_length, true);
	
	return PIN;
}
