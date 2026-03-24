#include "conceal.h"
#include "base64.h"
#include "compression.h"
#include "encryption.h"
#include "image.h"
#include "io_utils.h"
#include "profile_template.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <format>
#include <limits>
#include <print>
#include <span>
#include <stdexcept>
#include <system_error>

namespace {

constexpr std::size_t
	MAX_COMBINED_SIZE         = 1ULL * 1024 * 1024 * 1024,
	FILENAME_MAX_LEN          = 20,
	MAX_BLUESKY_UPLOAD_SIZE   = 1000000;

inline void writeLe32At(vBytes& v, std::size_t offset, std::uint32_t value) {
	requireSpanRange(v, offset, 4, "Internal Error: writeLe32At out of bounds.");
	v[offset + 0] = static_cast<Byte>(value & 0xFF);
	v[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
	v[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
	v[offset + 3] = static_cast<Byte>((value >> 24) & 0xFF);
}

inline void writeBe32At(vBytes& v, std::size_t offset, std::uint32_t value) {
	requireSpanRange(v, offset, 4, "Internal Error: writeBe32At out of bounds.");
	v[offset + 0] = static_cast<Byte>((value >> 24) & 0xFF);
	v[offset + 1] = static_cast<Byte>((value >> 16) & 0xFF);
	v[offset + 2] = static_cast<Byte>((value >> 8) & 0xFF);
	v[offset + 3] = static_cast<Byte>(value & 0xFF);
}

inline void writeLe24At(vBytes& v, std::size_t offset, std::uint32_t value) {
	requireSpanRange(v, offset, 3, "Internal Error: writeLe24At out of bounds.");
	v[offset + 0] = static_cast<Byte>(value & 0xFF);
	v[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
	v[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
}

struct OutputFileHandle {
	fs::path path{};
	int fd{-1};
};

[[nodiscard]] OutputFileHandle createUniqueOutputFile() {
	constexpr std::size_t MAX_ATTEMPTS = 2048;

	for (std::size_t i = 0; i < MAX_ATTEMPTS; ++i) {
		const uint32_t rand_num = 100000 + randombytes_uniform(900000);
		const fs::path candidate = std::format("wbpdv_{}.webp", rand_num);

		int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
		flags |= O_NOFOLLOW;
#endif
		const int fd = ::open(candidate.c_str(), flags, S_IRUSR | S_IWUSR);
		if (fd >= 0) {
			return OutputFileHandle{ .path = candidate, .fd = fd };
		}

		if (errno == EEXIST) {
			continue;
		}

		const std::error_code ec(errno, std::generic_category());
		throw std::runtime_error(std::format("Write Error: Unable to create output file: {}", ec.message()));
	}
	throw std::runtime_error("Write Error: Unable to allocate output filename.");
}

void writeOutputFile(std::span<const Byte> data, std::uint64_t pin, bool is_bluesky) {
	OutputFileHandle output_file = createUniqueOutputFile();

	try {
		writeAllToFd(output_file.fd, data);
		if (::fdatasync(output_file.fd) < 0) {
			const std::error_code ec(errno, std::generic_category());
			throw std::runtime_error(std::format("Write Error: Failed to sync output file: {}", ec.message()));
		}
		closeFdOrThrow(output_file.fd);
	} catch (...) {
		closeFdNoThrow(output_file.fd);
		std::error_code ec;
		fs::remove(output_file.path, ec);
		throw;
	}
	std::println("\nPlatform compatibility for output image:-\n");

	if (is_bluesky) {
		std::println(" ✓ Bluesky. (Only share this \"file-embedded\" WEBP image on Bluesky).");
	}
	else if (data.size() > 9 * 1024 * 1024) {
		std::println(" ✓ Mastodon");
	}
	else {
		std::println(" ✓ Mastodon");
		std::println(" ✓ Tumblr");
	}

	std::println("\nSaved \"file-embedded\" WEBP image: {} ({} bytes).", output_file.path.string(), data.size());
	std::println("\nRecovery PIN: [***{}***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n\nComplete!\n", pin);
}

} // namespace

void concealData(vBytes& image_vec, Option option, const fs::path& data_file_path) {
	const bool is_bluesky = (option == Option::Bluesky);

	// 1. Read and validate data file.
	const std::size_t data_file_size = getFileSizeChecked(data_file_path);
	const std::string data_filename = data_file_path.filename().string();

	if (data_filename.size() > FILENAME_MAX_LEN) {
		throw std::runtime_error("Data File Error: For compatibility requirements, length of data filename must not exceed 20 characters.");
	}

	// Validate and prepare the WebP image (strips headers, gets dimensions).
	const auto [width, height] = validateAndPrepareImage(image_vec);

	if (data_file_size > std::numeric_limits<std::size_t>::max() - image_vec.size()) {
		throw std::runtime_error("File Size Error: Combined size overflow.");
	}
	if (!is_bluesky && data_file_size + image_vec.size() > MAX_COMBINED_SIZE) {
		throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum size limit of 1GB.");
	}

	// 2. Read data file into vBytes.
	vBytes data_vec = readFile(data_file_path);

	// 3. Compress.
	zlibDeflate(data_vec);

	if (is_bluesky) {
		// --- Bluesky EXIF path (with optional XMP overflow) ---

		// 4b. Copy Bluesky EXIF template.
		vBytes profile_vec(WEBP_BLUESKY_EXIF_TEMPLATE.begin(), WEBP_BLUESKY_EXIF_TEMPLATE.end());

		// 5b. Write image dimensions (same offsets: 0x18, 0x1B).
		writeLe24At(profile_vec, 0x18, static_cast<std::uint32_t>(width - 1));
		writeLe24At(profile_vec, 0x1B, static_cast<std::uint32_t>(height - 1));

		// 6b. Encrypt.
		const std::uint64_t pin = encryptDataFile(profile_vec, data_vec, data_filename, BLUESKY_EMBED_OFFSETS);

		// 7b. Check if encrypted data exceeds EXIF chunk limit (~64KB).
		//     If so, split: EXIF gets first portion, XMP gets base64-encoded overflow.
		//     (Bluesky strips ICCP, so we only use EXIF + XMP.)
		const std::size_t encrypted_size = profile_vec.size() - BLUESKY_EMBED_OFFSETS.encrypted_file;
		vBytes xmp_chunk;

		if (encrypted_size > MAX_EXIF_ENCRYPTED_DATA) {
			const std::size_t exif_end = BLUESKY_EMBED_OFFSETS.encrypted_file + MAX_EXIF_ENCRYPTED_DATA;
			const std::size_t overflow_size = profile_vec.size() - exif_end;

			constexpr std::string_view XMP_HEADER =
				"<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
				"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Go XMP SDK 1.0\">"
				"<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
				"<rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\" rdf:about=\"\">"
				"<dc:creator><rdf:Seq><rdf:li>";
			constexpr std::string_view XMP_FOOTER =
				"</rdf:li></rdf:Seq></dc:creator></rdf:Description></rdf:RDF></x:xmpmeta>\n"
				"<?xpacket end=\"w\"?>";

			const std::size_t base64_size = ((overflow_size + 2) / 3) * 4;
			vBytes xmp_data;
			xmp_data.reserve(XMP_HEADER.size() + base64_size + XMP_FOOTER.size());
			xmp_data.insert(xmp_data.end(),
				reinterpret_cast<const Byte*>(XMP_HEADER.data()),
				reinterpret_cast<const Byte*>(XMP_HEADER.data() + XMP_HEADER.size()));

			std::span<const Byte> overflow(profile_vec.data() + exif_end, overflow_size);
			binaryToBase64(overflow, xmp_data);

			xmp_data.insert(xmp_data.end(),
				reinterpret_cast<const Byte*>(XMP_FOOTER.data()),
				reinterpret_cast<const Byte*>(XMP_FOOTER.data() + XMP_FOOTER.size()));

			const std::size_t xmp_payload = xmp_data.size();
			if (xmp_payload > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
				throw std::runtime_error("File Size Error: XMP chunk payload exceeds 4GB limit.");
			}
			xmp_chunk.resize(8);
			xmp_chunk[0]='X'; xmp_chunk[1]='M'; xmp_chunk[2]='P'; xmp_chunk[3]=' ';
			writeLe32At(xmp_chunk, 4, static_cast<std::uint32_t>(xmp_payload));
			xmp_chunk.insert(xmp_chunk.end(), xmp_data.begin(), xmp_data.end());

			// Truncate profile_vec to just the EXIF portion.
			profile_vec.resize(exif_end);

			// Set VP8X flags: EXIF (0x08) + XMP (0x04) = 0x0C.
			profile_vec[0x14] = 0x0C;
		}

		// 8b. Update EXIF chunk size.
		const std::size_t exif_data_size = profile_vec.size() - 0x26;
		if (exif_data_size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
			throw std::runtime_error("File Size Error: EXIF chunk data exceeds 4GB limit.");
		}
		writeLe32At(profile_vec, BLUESKY_EXIF_CHUNK_SIZE_OFFSET, static_cast<std::uint32_t>(exif_data_size));

		// 9b. Update Artist IFD count field (4-byte BE at offset 0x6A).
		constexpr std::size_t ARTIST_DATA_START = 0xB0;
		const std::size_t artist_count = profile_vec.size() - ARTIST_DATA_START;
		if (artist_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
			throw std::runtime_error("File Size Error: Artist IFD count exceeds 4GB limit.");
		}
		writeBe32At(profile_vec, BLUESKY_ARTIST_COUNT_OFFSET, static_cast<std::uint32_t>(artist_count));

		// 10b. Assemble output: RIFF+VP8X + VP8 image data + EXIF chunk + [XMP chunk].
		//       Place image data before metadata chunks so Bluesky doesn't need to reposition them.
		constexpr std::size_t VP8X_END = 0x1E;

		vBytes output_vec;
		output_vec.reserve(VP8X_END + image_vec.size() + (profile_vec.size() - VP8X_END) + xmp_chunk.size() + 2);

		// RIFF header + VP8X chunk.
		output_vec.insert(output_vec.end(), profile_vec.begin(), profile_vec.begin() + VP8X_END);

		// VP8 image data (before metadata chunks).
		output_vec.insert(output_vec.end(), image_vec.begin(), image_vec.end());

		// EXIF chunk (tag + size + data).
		output_vec.insert(output_vec.end(), profile_vec.begin() + VP8X_END, profile_vec.end());
		if (exif_data_size % 2 != 0) {
			output_vec.push_back(0x00);
		}

		// XMP chunk (if overflow).
		if (!xmp_chunk.empty()) {
			output_vec.insert(output_vec.end(), xmp_chunk.begin(), xmp_chunk.end());
			const std::size_t xmp_payload_size = xmp_chunk.size() - 8;
			if (xmp_payload_size % 2 != 0) {
				output_vec.push_back(0x00);
			}
		}

		// 11b. Write RIFF size and check Bluesky upload limit.
		const std::size_t total_size = output_vec.size();
		if (total_size < 8 || total_size - 8 > std::numeric_limits<std::uint32_t>::max()) {
			throw std::runtime_error("File Size Error: Output file too large for WEBP format.");
		}
		if (total_size > MAX_BLUESKY_UPLOAD_SIZE) {
			throw std::runtime_error(
				"File Size Error: Output file (" + std::to_string(total_size) +
				" bytes) exceeds the Bluesky upload limit of " +
				std::to_string(MAX_BLUESKY_UPLOAD_SIZE) + " bytes.\n"
				"Try a smaller cover image or data file.");
		}
		writeLe32At(output_vec, 0x04, static_cast<std::uint32_t>(total_size - 8));

		// 12b. Write output.
		writeOutputFile(std::span<const Byte>(output_vec.data(), output_vec.size()), pin, true);
		return;
	}

	// --- Default ICCP path ---

	// 4. Copy profile template.
	vBytes profile_vec(WEBP_PROFILE_TEMPLATE.begin(), WEBP_PROFILE_TEMPLATE.end());

	// 5. Write image dimensions into profile (width-1 at 0x18 as 3-byte LE, height-1 at 0x1B as 3-byte LE).
	writeLe24At(profile_vec, 0x18, static_cast<std::uint32_t>(width - 1));
	writeLe24At(profile_vec, 0x1B, static_cast<std::uint32_t>(height - 1));

	// 6. Encrypt.
	const std::uint64_t pin = encryptDataFile(profile_vec, data_vec, data_filename, WEBP_EMBED_OFFSETS);

	// 7. Compute ICCP chunk data size = profile_vec.size() - 0x26.
	//    The ICCP chunk data starts at offset 0x26 (after RIFF header + VP8X chunk + ICCP chunk header).
	const std::size_t iccp_data_size = profile_vec.size() - 0x26;
	if (iccp_data_size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
		throw std::runtime_error("File Size Error: ICCP chunk data exceeds 4GB limit.");
	}

	// WebP requires even chunk sizes. If odd, a 0x00 pad byte is inserted
	// BETWEEN the ICCP chunk and the VP8 chunk (not inside the profile data).
	const bool needs_pad = (iccp_data_size % 2 != 0);

	// 8. Write ICCP chunk size at offset 0x22 (4-byte LE) — always the actual data size (may be odd).
	writeLe32At(profile_vec, 0x22, static_cast<std::uint32_t>(iccp_data_size));

	// 9. Write ICC profile size at offset 0x26 (4-byte BE).
	writeBe32At(profile_vec, 0x26, static_cast<std::uint32_t>(iccp_data_size));

	// 10. Assemble: profile_vec + [optional pad byte] + image_vec.
	const std::size_t pad_size = needs_pad ? 1 : 0;
	profile_vec.reserve(profile_vec.size() + pad_size + image_vec.size());
	if (needs_pad) {
		profile_vec.push_back(0x00);
	}
	profile_vec.insert(profile_vec.end(), image_vec.begin(), image_vec.end());

	// 11. Write RIFF size at offset 0x04 (4-byte LE) = total_size - 8.
	const std::size_t total_size = profile_vec.size();
	if (total_size < 8 || total_size - 8 > std::numeric_limits<std::uint32_t>::max()) {
		throw std::runtime_error("File Size Error: Output file too large for WEBP format.");
	}
	writeLe32At(profile_vec, 0x04, static_cast<std::uint32_t>(total_size - 8));

	// 12. Write output file.
	writeOutputFile(std::span<const Byte>(profile_vec.data(), profile_vec.size()), pin, false);
}
