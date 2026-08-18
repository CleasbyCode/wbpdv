// libFuzzer entry point for wbpdv's untrusted-input parsers.
//
// Everything reachable here runs *before* any authentication, on bytes taken
// straight from a downloaded image, so this is the whole memory-safety surface
// of recover mode: the RIFF container walk, the ICC/EXIF payload detectors, the
// TIFF/IFD walker, and the XMP Base64 extractor.
//
// recoverData() itself is deliberately not the target. Reaching its interesting
// code needs a valid PIN, every candidate that gets that far pays a 64 MiB
// Argon2id derivation (destroying throughput), it reads stdin, and on success
// it writes a file into the working directory. Driving the parsers directly is
// fast, deterministic and side-effect free.
//
// recover.cpp is #included rather than linked so the anonymous-namespace
// parsers are reachable. Do NOT also link recover.cpp into this target.

#include "recover.cpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  // extractXmpOverflowData reaches libsodium's Base64 decoder.
  if (sodium_init() < 0) {
    __builtin_trap();
  }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  const std::span<const Byte> input(data, size);
  try {
    const std::span<const Byte> riff_file = parseRiffWebP(input);
    const RiffChunkSet chunks = findRelevantChunks(riff_file);

    if (chunks.iccp.has_value()) {
      (void)hasEmbeddedIccPayload(*chunks.iccp);
    }
    if (chunks.exif.has_value()) {
      (void)hasEmbeddedBlueskyPayload(*chunks.exif);
      // Independent of the detector: the walker must stay in bounds on EXIF
      // data the detector would have rejected.
      (void)findExifArtistEnd(*chunks.exif);
    }
    if (chunks.xmp.has_value()) {
      (void)extractXmpOverflowData(*chunks.xmp);
    }
  } catch (const std::exception &) {
    // Rejecting malformed input is the expected outcome. Only crashes,
    // sanitizer reports, leaks and hangs count as failures.
  }
  return 0;
}
