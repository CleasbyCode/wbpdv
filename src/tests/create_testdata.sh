#!/bin/bash
# Create deterministic payloads and a cover WEBP for wbpdv golden-file tests.
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
# 96x96 RGBA gradient encoded to a small lossy VP8 WEBP via libwebp. Kept small
# so the same cover works for standard mode and for Bluesky's 1,000,000-byte
# output cap. Emitted by a one-off generator so the fixtures need no cwebp.
COVER="$DATA/covers/cover.webp"
create_cover() (
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
    FILE* f = std::fopen("cover.webp", "wb");
    if (f == nullptr) { WebPFree(out); return 2; }
    const size_t wrote = std::fwrite(out, 1, n, f);
    std::fclose(f);
    WebPFree(out);
    return wrote == n ? 0 : 3;
}
CPP
    (
        cd "$tmp"
        g++ -std=c++23 -O2 $(pkg-config --cflags libwebp) make_cover.cpp \
            $(pkg-config --libs libwebp) -o make_cover
        ./make_cover
    )
    cp "$tmp/cover.webp" "$COVER"
)

if [[ ! -f "$COVER" ]]; then
    create_cover
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
