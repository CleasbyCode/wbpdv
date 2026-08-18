#!/bin/bash
# libFuzzer harness for wbpdv's recover-mode parsers.
#
# Builds tests/fuzz/fuzz_recover.cpp with ASan+UBSan and runs it over a seed
# corpus derived from the golden embedded images and the test covers. Requires
# clang++ (libFuzzer ships with it; g++ does not support -fsanitize=fuzzer).
#
#   bash tests/run_fuzz.sh                  # 60s smoke run, used by CI
#   bash tests/run_fuzz.sh --time 3600      # longer soak
#   bash tests/run_fuzz.sh --no-build       # reuse the existing binary
#   bash tests/run_fuzz.sh -- -jobs=4       # pass extra libFuzzer flags
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TESTS/.." && pwd)"
FUZZ_DIR="$TESTS/fuzz"
BIN="${WBPDV_FUZZ_BIN:-$FUZZ_DIR/wbpdv_fuzz_recover}"
CORPUS="${WBPDV_FUZZ_CORPUS:-$FUZZ_DIR/corpus}"
SEEDS="$FUZZ_DIR/seeds"
ARTIFACTS="$FUZZ_DIR/artifacts"
RUN_TIME=60
NO_BUILD=0
EXTRA_ARGS=()

usage() {
    cat <<'EOF'
Usage: tests/run_fuzz.sh [options] [-- <libfuzzer args>]

Options:
  --time <seconds>  Run duration (default 60; 0 runs until stopped).
  --no-build        Reuse the existing fuzz binary.
  --bin <path>      Use an explicit binary path.
  -h, --help        Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --time)
            if [[ $# -lt 2 || ! "${2-}" =~ ^[0-9]+$ ]]; then
                echo "Option --time requires a non-negative integer." >&2
                exit 2
            fi
            RUN_TIME="$2"; shift 2;;
        --no-build) NO_BUILD=1; shift;;
        --bin)
            if [[ $# -lt 2 || -z "${2-}" ]]; then
                echo "Option --bin requires a path." >&2
                exit 2
            fi
            BIN="$2"; NO_BUILD=1; shift 2;;
        -h|--help) usage; exit 0;;
        --) shift; EXTRA_ARGS=("$@"); break;;
        *) echo "Unknown option: $1" >&2; usage; exit 2;;
    esac
done

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

CXX="${FUZZ_CXX:-clang++}"

if [[ "$NO_BUILD" -eq 0 ]]; then
    need_cmd "$CXX"
    need_cmd pkg-config

    if ! "$CXX" --version 2>/dev/null | grep -qi clang; then
        echo "Fuzzing requires clang++ (-fsanitize=fuzzer). Set FUZZ_CXX." >&2
        exit 1
    fi

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

    # recover.cpp is #included by the harness so its anonymous-namespace parsers
    # are reachable; linking it again would duplicate every external symbol.
    SOURCES=(
        "$FUZZ_DIR/fuzz_recover.cpp"
        "$ROOT/base64.cpp"
        "$ROOT/compression.cpp"
        "$ROOT/encryption.cpp"
        "$ROOT/io_utils.cpp"
    )

    mkdir -p "$(dirname "$BIN")"
    echo "Compiling fuzz target -> $BIN"
    "$CXX" -std=c++23 -O1 -g \
        -fsanitize=fuzzer,address,undefined \
        -fno-sanitize-recover=all \
        -fno-omit-frame-pointer \
        -Wall -Wextra \
        -I"$ROOT" \
        "${WEBP_CFLAGS[@]}" \
        "${SOURCES[@]}" \
        "${WEBP_LDFLAGS[@]}" \
        -lsodium -lz -ldeflate -lwebp \
        -o "$BIN"
fi

if [[ ! -x "$BIN" ]]; then
    echo "Fuzz binary not found or not executable: $BIN" >&2
    exit 1
fi

# Seed corpus: real embedded images exercise the ICC and EXIF+XMP paths, plain
# covers exercise the container walk with no wbpdv metadata at all. Without
# these the fuzzer burns its budget rediscovering the "RIFF"/"WEBP" magic.
mkdir -p "$SEEDS" "$CORPUS" "$ARTIFACTS"
seed_count=0
while IFS= read -r -d '' seed; do
    cp -f -- "$seed" "$SEEDS/$(printf '%s' "${seed#"$TESTS/"}" | tr '/' '_')"
    seed_count=$((seed_count + 1))
done < <(find "$TESTS/golden" "$TESTS/testdata/covers" -type f -name '*.webp' \
             -print0 2>/dev/null)

if [[ "$seed_count" -eq 0 ]]; then
    echo "Warning: no seed images found; run tests/create_testdata.sh first." >&2
fi
echo "Seeds: $seed_count  Corpus: $CORPUS"

FUZZ_ARGS=(
    "$CORPUS" "$SEEDS"
    "-artifact_prefix=$ARTIFACTS/"
    -print_final_stats=1
    # Recover only ever sees whole image files, and readFile caps those well
    # below this; a bound keeps the fuzzer on realistic inputs.
    -max_len=262144
    -rss_limit_mb=4096
)
if [[ "$RUN_TIME" -gt 0 ]]; then
    FUZZ_ARGS+=("-max_total_time=$RUN_TIME")
fi

echo "Running $BIN (${RUN_TIME}s)"
if ! "$BIN" "${FUZZ_ARGS[@]}" "${EXTRA_ARGS[@]}"; then
    echo
    echo "Fuzzing FAILED. Reproducer(s) under: $ARTIFACTS" >&2
    exit 1
fi

echo
echo "Fuzz run clean. Corpus: $CORPUS"
