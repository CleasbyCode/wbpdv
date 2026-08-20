#!/bin/bash
# Create deterministic payloads and cover WEBPs for wbpdv tests.
#
# Dependencies: g++ + libwebp (compiles a tiny WebPEncodeRGBA generator, so no
# image tooling like cwebp/ffmpeg is required) and python3 (seeded payloads).
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA="$TESTS/testdata"
mkdir -p "$DATA/covers" "$DATA/payloads"

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

need_cmd g++
need_cmd python3
need_cmd pkg-config

# --- Cover WEBP ------------------------------------------------------------
# The main 96x96 RGBA gradient is encoded to a small lossy VP8 WEBP. A lossless
# alpha variant exercises the compatibility re-encode path. Emitted by a
# one-off generator so the fixtures need no cwebp.
COVER="$DATA/covers/cover.webp"
COVER_LOSSLESS_ALPHA="$DATA/covers/cover_lossless_alpha.webp"
create_covers() (
    tmp="$(mktemp -d)"
    trap 'rm -rf -- "$tmp"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    cat > "$tmp/make_cover.cpp" <<'CPP'
#include <webp/encode.h>
#include <cstddef>
#include <cstdio>
#include <vector>

bool write_webp(const char* path, unsigned char* data, std::size_t size) {
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        WebPFree(data);
        return false;
    }
    const std::size_t wrote = std::fwrite(data, 1, size, f);
    const int close_status = std::fclose(f);
    WebPFree(data);
    return wrote == size && close_status == 0;
}

int main() {
    const int w = 96, h = 96;
    std::vector<unsigned char> rgba(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            rgba[i + 0] = static_cast<unsigned char>((x * 3 + y) & 0xFF);
            rgba[i + 1] = static_cast<unsigned char>((y * 5 + x) & 0xFF);
            rgba[i + 2] = static_cast<unsigned char>((x + y * 2) & 0xFF);
            rgba[i + 3] = 255;
        }
    unsigned char* out = nullptr;
    const size_t n = WebPEncodeRGBA(rgba.data(), w, h, w * 4, 75.0f, &out);
    if (n == 0 || out == nullptr) return 1;
    if (!write_webp("cover.webp", out, n)) return 2;

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            rgba[i + 3] = static_cast<unsigned char>((x * 7 + y * 5) & 0xFF);
        }
    out = nullptr;
    const size_t lossless_n =
        WebPEncodeLosslessRGBA(rgba.data(), w, h, w * 4, &out);
    if (lossless_n == 0 || out == nullptr) return 3;
    return write_webp("cover_lossless_alpha.webp", out, lossless_n) ? 0 : 4;
}
CPP
    (
        cd "$tmp"
        g++ -std=c++23 -O2 $(pkg-config --cflags libwebp) make_cover.cpp \
            $(pkg-config --libs libwebp) -o make_cover
        ./make_cover
    )
    cp "$tmp/cover.webp" "$COVER"
    cp "$tmp/cover_lossless_alpha.webp" "$COVER_LOSSLESS_ALPHA"
)

if [[ ! -f "$COVER" || ! -f "$COVER_LOSSLESS_ALPHA" ]]; then
    create_covers
fi

# --- Deterministic payloads ------------------------------------------------
# Seeded RNG so bytes are reproducible. payload_bsky.bin is sized so that, once
# compressed+encrypted, it exceeds the 65000-byte EXIF artist field and spills
# into the XMP overflow chunk (random data does not compress).
DATA="$DATA" python3 - <<'PY'
import os, random
from pathlib import Path

root = Path(os.environ["DATA"]) / "payloads"
root.mkdir(parents=True, exist_ok=True)

text = root / "payload_text.txt"
if not text.exists():
    text.write_bytes(
        b"wbpdv golden test payload.\n"
        b"The quick brown fox jumps over the lazy dog.\n"
        b"Line three.\n"
    )

specs = {
    "payload_bin.bin":  (200_000, 42),
    "payload_bsky.bin": (80_000,  43),
}
for name, (size, seed) in specs.items():
    p = root / name
    if p.exists() and p.stat().st_size == size:
        continue
    rng = random.Random(seed)
    p.write_bytes(bytes(rng.randrange(256) for _ in range(size)))
PY

echo "Testdata ready under: $DATA"
