uint32_t encryptFile(std::vector<uint8_t>& Profile_Vec, std::vector<uint8_t>& File_Vec, uint32_t data_file_size, std::string& data_filename) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
	
	constexpr uint8_t 
		XOR_KEY_LENGTH = 234,
		PIN_LENGTH = 4;

	uint16_t 
		data_file_start_index = 0x37C,
		profile_data_size = 854,
		cover_index = 0x35A,
		deflate_file_size_index = 0x36F,
		data_filename_index = cover_index + 1,
		xor_key_index = 0x270,
		pin_index = data_file_start_index,
		encrypt_xor_pos = xor_key_index,
		index_xor_pos = encrypt_xor_pos;

	uint32_t
		iccp_chunksize = data_file_size + profile_data_size,
		index_pos = 0;

	uint8_t 
		iccp_chunk_size_field_index = 0x25,
		profile_size_field_index = 0x26,
		data_filename_length = Profile_Vec[cover_index],
		xor_key[XOR_KEY_LENGTH],
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
		constexpr uint16_t EXTRA_BYTE_FLAG_INDEX = 0x373;
		Profile_Vec.insert(Profile_Vec.begin() + pin_index, 0x19);
		++pin_index;
		++data_file_start_index;
		++iccp_chunksize;
		Profile_Vec[EXTRA_BYTE_FLAG_INDEX] = 0x01;
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
		data_filename[char_pos] = data_filename[char_pos] ^ xor_key[xor_key_pos++];
		Profile_Vec[data_filename_index++] = data_filename[char_pos++];
	}	
	
	// write deflate file size to our profile
	valueUpdater(Profile_Vec, deflate_file_size_index, data_file_size, value_bit_length, true);

	Profile_Vec.reserve(Profile_Vec.size() + data_file_size);

	while (data_file_size--) {
		Profile_Vec.emplace_back(File_Vec[index_pos++] ^ xor_key[xor_key_pos++ % XOR_KEY_LENGTH]);
	}
	
	while(xor_key_length--) {
		Profile_Vec[encrypt_xor_pos++] = Profile_Vec[index_xor_pos++] ^ Profile_Vec[pin_index++];
		pin_index = pin_index >= PIN_LENGTH + data_file_start_index ? data_file_start_index : pin_index;
	}
	
	pin_index = data_file_start_index;

	const uint32_t 
		PIN = getByteValue(Profile_Vec, pin_index),
		COVER_PIN = getByteValue(Profile_Vec, cover_index);
	
	valueUpdater(Profile_Vec, pin_index, COVER_PIN, value_bit_length, true);
	
	return PIN;
}