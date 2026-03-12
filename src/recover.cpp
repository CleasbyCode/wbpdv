#include "recover.h"
#include "base64.h"
#include "compression.h"
#include "encryption.h"
#include "io_utils.h"

#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <format>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

[[nodiscard]] std::optional<std::size_t> searchSig(const vBytes& vec, const auto& sig) {
	if (vec.size() < sig.size()) {
		return std::nullopt;
	}
	auto it = std::search(vec.begin(), vec.end(), sig.begin(), sig.end());
	if (it == vec.end()) {
		return std::nullopt;
	}
	return static_cast<std::size_t>(std::distance(vec.begin(), it));
}

[[nodiscard]] fs::path safeRecoveryPath(std::string decrypted_filename) {
	if (decrypted_filename.empty()) {
		throw std::runtime_error("File Recovery Error: Recovered filename is unsafe.");
	}

	fs::path parsed(std::move(decrypted_filename));
	if (parsed.has_root_path() ||
		parsed.has_parent_path() ||
		parsed != parsed.filename() ||
		!hasValidFilename(parsed)) {
		throw std::runtime_error("File Recovery Error: Recovered filename is unsafe.");
	}

	fs::path candidate = parsed.filename();
	std::error_code ec;
	if (!fs::exists(candidate, ec)) {
		return candidate;
	}

	std::string stem = candidate.stem().string();
	if (stem.empty()) {
		stem = "recovered";
	}
	const std::string ext = candidate.extension().string();
	std::string next_name;
	next_name.reserve(stem.size() + 1 + 20 + ext.size());

	for (std::size_t i = 1; i <= 10000; ++i) {
		std::array<char, 32> i_buf{};
		const auto [ptr, ec_to_chars] = std::to_chars(i_buf.data(), i_buf.data() + i_buf.size(), i);
		if (ec_to_chars != std::errc{}) {
			throw std::runtime_error("Write File Error: Unable to create a unique output filename.");
		}

		next_name.clear();
		next_name.append(stem);
		next_name.push_back('_');
		next_name.append(i_buf.data(), ptr);
		next_name.append(ext);

		fs::path next(next_name);
		if (!fs::exists(next, ec)) {
			return next;
		}
	}
	throw std::runtime_error("Write File Error: Unable to create a unique output filename.");
}

struct StagedOutputFile {
	fs::path path{};
	int fd{-1};
};

[[nodiscard]] StagedOutputFile createStagedOutputFile(const fs::path& output_path) {
	constexpr std::size_t MAX_ATTEMPTS = 1024;
	const fs::path parent = output_path.parent_path();
	const std::string base = output_path.filename().string();

	const std::string prefix = std::format(".{}.wbpdv_tmp_", base);
	for (std::size_t i = 0; i < MAX_ATTEMPTS; ++i) {
		const uint32_t rand_num = 100000 + randombytes_uniform(900000);
		const fs::path candidate = parent / std::format("{}{}", prefix, rand_num);

		int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
		flags |= O_NOFOLLOW;
#endif

		const int fd = ::open(candidate.c_str(), flags, S_IRUSR | S_IWUSR);
		if (fd >= 0) {
			return StagedOutputFile{ .path = candidate, .fd = fd };
		}

		if (errno == EEXIST) {
			continue;
		}

		const std::error_code ec(errno, std::generic_category());
		throw std::runtime_error(std::format("Write File Error: Unable to create temp output file: {}", ec.message()));
	}
	throw std::runtime_error("Write File Error: Unable to allocate temporary output filename.");
}

void syncParentDirectory(const fs::path& file_path) {
	fs::path dir = file_path.parent_path();
	if (dir.empty()) {
		dir = ".";
	}
	const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
	if (dir_fd >= 0) {
		::fsync(dir_fd);
		::close(dir_fd);
	}
}

void commitRecoveredOutput(const fs::path& staged_path, const fs::path& output_path) {
#ifdef __linux__
	const long rename_rc = ::syscall(
		SYS_renameat2,
		AT_FDCWD, staged_path.c_str(),
		AT_FDCWD, output_path.c_str(),
		RENAME_NOREPLACE
	);
	if (rename_rc == 0) {
		syncParentDirectory(output_path);
		return;
	}
	if (errno != ENOSYS && errno != EINVAL) {
		if (errno == EEXIST) {
			throw std::runtime_error("Write File Error: Output file already exists.");
		}
		const std::error_code ec(errno, std::generic_category());
		throw std::runtime_error(std::format("Write File Error: Failed to commit recovered file: {}", ec.message()));
	}
#endif

	std::error_code ec;
	if (fs::exists(output_path, ec)) {
		throw std::runtime_error("Write File Error: Output file already exists.");
	}
	if (ec) {
		throw std::runtime_error(std::format("Write File Error: Failed to check output path: {}", ec.message()));
	}
	fs::rename(staged_path, output_path, ec);
	if (ec) {
		throw std::runtime_error(std::format("Write File Error: Failed to commit recovered file: {}", ec.message()));
	}
	syncParentDirectory(output_path);
}

void writeRecoveredFile(vBytes& image_vec, std::string filename) {
	fs::path output_path = safeRecoveryPath(std::move(filename));
	StagedOutputFile staged_file = createStagedOutputFile(output_path);

	try {
		writeAllToFd(staged_file.fd, std::span<const Byte>(image_vec.data(), image_vec.size()));
		if (::fdatasync(staged_file.fd) < 0) {
			const std::error_code ec(errno, std::generic_category());
			throw std::runtime_error(std::format("Write File Error: Failed to sync output file: {}", ec.message()));
		}
		closeFdOrThrow(staged_file.fd);
		commitRecoveredOutput(staged_file.path, output_path);
	} catch (...) {
		closeFdNoThrow(staged_file.fd);
		cleanupPathNoThrow(staged_file.path);
		throw;
	}

	std::println("\nExtracted hidden file: {} ({} bytes).\n\nComplete! Please check your file.\n",
		output_path.string(), image_vec.size());
}

void recoverFromIccPath(vBytes& image_vec) {
	// Search for ICC_PROFILE_SIGNATURE to find profile start.
	auto icc_pos = searchSig(image_vec, ICC_PROFILE_SIGNATURE);
	if (!icc_pos.has_value()) {
		throw std::runtime_error("Image File Error: Cannot locate ICC profile in image.");
	}

	// Read the ICCP chunk size to know profile extent, then extract profile data.
	constexpr std::size_t ICCP_SIZE_OFFSET = 0x22;
	if (image_vec.size() < ICCP_SIZE_OFFSET + 4) {
		throw std::runtime_error("Image File Error: Image too small to contain ICCP chunk.");
	}
	const std::size_t iccp_chunk_size =
		static_cast<std::size_t>(image_vec[ICCP_SIZE_OFFSET]) |
		(static_cast<std::size_t>(image_vec[ICCP_SIZE_OFFSET + 1]) << 8) |
		(static_cast<std::size_t>(image_vec[ICCP_SIZE_OFFSET + 2]) << 16) |
		(static_cast<std::size_t>(image_vec[ICCP_SIZE_OFFSET + 3]) << 24);

	if (*icc_pos < ICC_SIG_OFFSET_FROM_PROFILE_START) {
		throw std::runtime_error("Image File Error: Corrupt profile location.");
	}
	const std::size_t profile_start = *icc_pos - ICC_SIG_OFFSET_FROM_PROFILE_START;

	constexpr std::size_t ICCP_DATA_START = 0x26;
	if (profile_start < ICCP_DATA_START || iccp_chunk_size < (profile_start - ICCP_DATA_START)) {
		throw std::runtime_error("Image File Error: Corrupt profile location.");
	}
	const std::size_t profile_data_length = iccp_chunk_size - (profile_start - ICCP_DATA_START);

	// Erase before profile start and truncate after profile data.
	image_vec.erase(image_vec.begin(), image_vec.begin() + static_cast<std::ptrdiff_t>(profile_start));
	if (image_vec.size() > profile_data_length) {
		image_vec.resize(profile_data_length);
	}

	// Verify KDF metadata at WEBP_EXTRACT_OFFSETS.kdf_metadata.
	requireSpanRange(image_vec, WEBP_EXTRACT_OFFSETS.kdf_metadata, KDF_METADATA_REGION_BYTES,
		"File Recovery Error: Embedded profile is corrupt.");

	// Decrypt.
	auto result = decryptDataFile(image_vec, WEBP_EXTRACT_OFFSETS);
	if (!result) {
		throw std::runtime_error("File Recovery Error: Invalid PIN or file is corrupt.");
	}

	// Decompress.
	zlibInflate(image_vec);

	// Write output.
	writeRecoveredFile(image_vec, std::move(*result));
}

[[nodiscard]] std::uint32_t readBe32(const vBytes& v, std::size_t offset) {
	return (static_cast<std::uint32_t>(v[offset]) << 24) |
		   (static_cast<std::uint32_t>(v[offset + 1]) << 16) |
		   (static_cast<std::uint32_t>(v[offset + 2]) << 8) |
		    static_cast<std::uint32_t>(v[offset + 3]);
}

// Extract base64-encoded overflow data from an XMP chunk in the file.
// Returns decoded binary data, or empty vector if no XMP chunk found.
[[nodiscard]] vBytes extractXmpOverflowData(const vBytes& file_vec) {
	constexpr const char* XMP_ERROR = "File Extraction Error: Corrupt XMP data.";

	auto xmp_tag_pos = searchSig(file_vec, XMP_CHUNK_TAG);
	if (!xmp_tag_pos) {
		return {};
	}

	// Find "<rdf:li" within the XMP data.
	auto rdfli_pos = searchSig(file_vec, XMP_RDFLI_SIGNATURE);
	if (!rdfli_pos || *rdfli_pos <= *xmp_tag_pos) {
		return {};
	}

	// Base64 starts after "<rdf:li>" (7 + 1 = 8 bytes past the signature start).
	const std::size_t base64_begin = *rdfli_pos + XMP_RDFLI_SIGNATURE.size() + 1;
	if (base64_begin >= file_vec.size()) {
		throw std::runtime_error(XMP_ERROR);
	}

	// Base64 ends at the next '<' delimiter.
	auto end_it = std::find(
		file_vec.begin() + static_cast<std::ptrdiff_t>(base64_begin),
		file_vec.end(),
		XMP_BASE64_END_DELIM);
	if (end_it == file_vec.end()) {
		throw std::runtime_error(XMP_ERROR);
	}

	const std::size_t base64_end = static_cast<std::size_t>(std::distance(file_vec.begin(), end_it));
	if (base64_end <= base64_begin) {
		throw std::runtime_error(XMP_ERROR);
	}

	std::span<const Byte> base64_span(file_vec.data() + base64_begin, base64_end - base64_begin);
	vBytes decoded;
	appendBase64AsBinary(base64_span, decoded);
	return decoded;
}

// Bluesky rearranges the TIFF structure: the Artist data offset and the
// order of sub-IFD/rational data may change. We anchor off the WDV signature
// (which is always at a fixed position within the Artist data) and read
// the Artist IFD count to determine the encrypted data boundary.
// If an XMP chunk exists, its base64 data is decoded and appended as overflow.
void recoverFromBlueskyPath(vBytes& image_vec) {
	constexpr const char* CORRUPT_ERROR = "File Recovery Error: Embedded EXIF data is corrupt.";

	// 1. Extract XMP overflow data BEFORE modifying image_vec.
	vBytes xmp_overflow = extractXmpOverflowData(image_vec);

	// 2. Find "Exif\0\0" header to locate EXIF data start.
	auto exif_pos = searchSig(image_vec, EXIF_HEADER_SIGNATURE);
	if (!exif_pos.has_value()) {
		throw std::runtime_error("Image File Error: Cannot locate EXIF data in image.");
	}

	// 3. Read EXIF chunk size from 4 bytes before the EXIF data.
	if (*exif_pos < 4) {
		throw std::runtime_error("Image File Error: Corrupt EXIF chunk location.");
	}
	const std::size_t size_offset = *exif_pos - 4;
	if (image_vec.size() < size_offset + 4) {
		throw std::runtime_error("Image File Error: Image too small to contain EXIF chunk.");
	}
	const std::size_t exif_chunk_size =
		static_cast<std::size_t>(image_vec[size_offset]) |
		(static_cast<std::size_t>(image_vec[size_offset + 1]) << 8) |
		(static_cast<std::size_t>(image_vec[size_offset + 2]) << 16) |
		(static_cast<std::size_t>(image_vec[size_offset + 3]) << 24);

	// 4. Erase everything before the EXIF data start ("Exif\0\0").
	image_vec.erase(image_vec.begin(), image_vec.begin() + static_cast<std::ptrdiff_t>(*exif_pos));
	if (image_vec.size() > exif_chunk_size) {
		image_vec.resize(exif_chunk_size);
	}

	// 5. Find WDV signature within the EXIF data.
	auto wdv_pos = searchSig(image_vec, WDV_SIGNATURE);
	if (!wdv_pos.has_value()) {
		throw std::runtime_error(CORRUPT_ERROR);
	}

	// 6. Compute KDF and encrypted data offsets relative to WDV.
	//    Layout within Artist data: padding(16) + KDF(56) + reserved(4) + WDV(7+1) + encrypted...
	//    So KDF is 60 bytes before WDV, encrypted is 8 bytes after WDV.
	constexpr std::size_t WDV_TO_KDF = 60; // 56 (KDF size) + 4 (reserved)
	constexpr std::size_t WDV_PLUS_TRAILING = 8; // 7 sig + 1 trailing byte

	if (*wdv_pos < WDV_TO_KDF) {
		throw std::runtime_error(CORRUPT_ERROR);
	}
	const std::size_t kdf_offset = *wdv_pos - WDV_TO_KDF;
	const std::size_t encrypted_offset = *wdv_pos + WDV_PLUS_TRAILING;

	// 7. Find Artist IFD entry (tag 0x013B) to determine data boundary.
	//    Bluesky may rearrange TIFF metadata after the Artist data, so we
	//    truncate to artist_offset + artist_count to exclude trailing data.
	constexpr std::size_t TIFF_HEADER_OFFSET = 6; // "Exif\0\0" is 6 bytes
	constexpr std::size_t IFD_COUNT_OFFSET = TIFF_HEADER_OFFSET + 8;
	requireSpanRange(image_vec, IFD_COUNT_OFFSET, 2, CORRUPT_ERROR);

	const std::uint16_t ifd_count = static_cast<std::uint16_t>(
		(static_cast<unsigned>(image_vec[IFD_COUNT_OFFSET]) << 8) | image_vec[IFD_COUNT_OFFSET + 1]);
	constexpr std::size_t MAX_IFD_ENTRIES = 64;
	if (ifd_count == 0 || ifd_count > MAX_IFD_ENTRIES) {
		throw std::runtime_error(CORRUPT_ERROR);
	}

	for (std::uint16_t i = 0; i < ifd_count; ++i) {
		const std::size_t entry_off = IFD_COUNT_OFFSET + 2 + static_cast<std::size_t>(i) * 12;
		requireSpanRange(image_vec, entry_off, 12, CORRUPT_ERROR);

		const std::uint16_t tag = static_cast<std::uint16_t>(
			(static_cast<unsigned>(image_vec[entry_off]) << 8) | image_vec[entry_off + 1]);

		if (tag == 0x013B) { // Artist tag
			const std::uint32_t artist_count = readBe32(image_vec, entry_off + 4);
			const std::uint32_t artist_tiff_offset = readBe32(image_vec, entry_off + 8);
			const std::size_t artist_end = TIFF_HEADER_OFFSET
				+ static_cast<std::size_t>(artist_tiff_offset)
				+ static_cast<std::size_t>(artist_count);

			if (artist_end > image_vec.size()) {
				throw std::runtime_error(CORRUPT_ERROR);
			}
			image_vec.resize(artist_end);
			break;
		}
	}

	// 8. Append XMP overflow data (decoded from base64) after the EXIF encrypted portion.
	if (!xmp_overflow.empty()) {
		image_vec.insert(image_vec.end(), xmp_overflow.begin(), xmp_overflow.end());
	}

	// 9. Verify KDF metadata and decrypt.
	const ProfileOffsets bluesky_offsets = { kdf_offset, encrypted_offset };
	requireSpanRange(image_vec, bluesky_offsets.kdf_metadata, KDF_METADATA_REGION_BYTES, CORRUPT_ERROR);

	auto result = decryptDataFile(image_vec, bluesky_offsets);
	if (!result) {
		throw std::runtime_error("File Recovery Error: Invalid PIN or file is corrupt.");
	}

	// 10. Decompress.
	zlibInflate(image_vec);

	// 11. Write output.
	writeRecoveredFile(image_vec, std::move(*result));
}

} // namespace

void recoverData(vBytes& image_vec) {
	// 1. Search for WDV_SIGNATURE in image_vec.
	auto wdv_pos = searchSig(image_vec, WDV_SIGNATURE);
	if (!wdv_pos.has_value()) {
		throw std::runtime_error("Image File Error: Signature check failure. This is not a valid wbpdv file-embedded image.");
	}

	// 2. Determine mode: if WDV signature is inside the ICCP chunk → default ICCP path.
	//    Otherwise → Bluesky EXIF path.
	//    (Platforms like Mastodon may add an EXIF chunk to ICCP-mode images,
	//    so we must check the ICCP chunk boundary, not just whether EXIF exists.)
	auto iccp_tag_pos = searchSig(image_vec, ICCP_CHUNK_TAG);
	bool wdv_in_iccp = false;

	if (iccp_tag_pos.has_value() && image_vec.size() >= *iccp_tag_pos + 8) {
		const std::size_t chunk_size =
			static_cast<std::size_t>(image_vec[*iccp_tag_pos + 4]) |
			(static_cast<std::size_t>(image_vec[*iccp_tag_pos + 5]) << 8) |
			(static_cast<std::size_t>(image_vec[*iccp_tag_pos + 6]) << 16) |
			(static_cast<std::size_t>(image_vec[*iccp_tag_pos + 7]) << 24);

		const std::size_t chunk_data_start = *iccp_tag_pos + 8;
		const std::size_t chunk_data_end = chunk_data_start + chunk_size;
		wdv_in_iccp = (*wdv_pos >= chunk_data_start && *wdv_pos < chunk_data_end);
	}

	if (wdv_in_iccp) {
		// Default ICCP path (WDV signature is in the ICC profile data).
		recoverFromIccPath(image_vec);
	} else {
		// Bluesky EXIF path (may also have XMP overflow chunk).
		recoverFromBlueskyPath(image_vec);
	}
}
