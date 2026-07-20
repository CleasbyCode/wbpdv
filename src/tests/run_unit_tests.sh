#!/bin/bash
# Module-level unit tests for wbpdv (endian, base64, filenames, crypto, EXIF).
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TESTS/.." && pwd)"
BIN="${WBPDV_UNIT_BIN:-$TESTS/unit/wbpdv_unit_tests}"
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: tests/run_unit_tests.sh [options]

Options:
  --no-build    Reuse existing unit-test binary.
  --bin <path>  Use an explicit binary path.
  -h, --help    Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build) NO_BUILD=1; shift;;
        --bin)
            if [[ $# -lt 2 || -z "${2-}" ]]; then
                echo "Option --bin requires a path." >&2
                usage
                exit 2
            fi
            BIN="$2"; NO_BUILD=1; shift 2;;
        -h|--help) usage; exit 0;;
        *) echo "Unknown option: $1" >&2; usage; exit 2;;
    esac
done

CXX="${CXX:-g++}"

if [[ "$NO_BUILD" -eq 0 ]]; then
    WEBP_CFLAGS_STR="$(pkg-config --cflags libwebp 2>/dev/null || true)"
    WEBP_LIBDIR_STR="$(pkg-config --libs-only-L libwebp 2>/dev/null || true)"
    WEBP_CFLAGS=()
    WEBP_LDFLAGS=()
    if [[ -n "$WEBP_CFLAGS_STR" ]]; then
        # shellcheck disable=SC2206
        WEBP_CFLAGS=( $WEBP_CFLAGS_STR )
    fi
    if [[ -n "$WEBP_LIBDIR_STR" ]]; then
        # shellcheck disable=SC2206
        WEBP_LDFLAGS=( $WEBP_LIBDIR_STR )
    fi

    mapfile -t SOURCES < <(find "$ROOT" -maxdepth 1 -name '*.cpp' ! -name 'main.cpp' | sort)
    SOURCES+=( "$TESTS/unit/unit_tests.cpp" )

    mkdir -p "$(dirname "$BIN")"
    echo "Compiling unit tests -> $BIN"
    "$CXX" -std=c++23 -O1 -g -Wall -Wextra -Wpedantic \
        -I"$ROOT" \
        "${WEBP_CFLAGS[@]}" \
        "${SOURCES[@]}" \
        "${WEBP_LDFLAGS[@]}" \
        -lsodium -lz -ldeflate -lwebp \
        -o "$BIN"
fi

if [[ ! -x "$BIN" ]]; then
    echo "Unit test binary not found or not executable: $BIN" >&2
    exit 1
fi

echo "Running $BIN"
"$BIN"
