#!/bin/bash
# Fresh conceal/recover regression tests for wbpdv.
#
# These are intentionally non-golden: every case runs conceal, parses the emitted
# PIN and output WEBP, runs recover, and compares the recovered bytes with the
# original payload.
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TESTS/.." && pwd)"
BIN="${WBPDV_BIN:-$ROOT/wbpdv}"
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: tests/run_roundtrip_tests.sh [options]

Options:
  --no-build    Reuse existing wbpdv binary.
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

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

absolute_path() {
    local path="$1"
    if [[ "$path" == /* ]]; then
        printf '%s\n' "$path"
        return 0
    fi

    local dir base
    dir="$(dirname "$path")"
    base="$(basename "$path")"
    if [[ ! -d "$dir" ]]; then
        printf '%s\n' "$path"
        return 0
    fi
    printf '%s/%s\n' "$(cd "$dir" && pwd -P)" "$base"
}

extract_embedded_image() {
    sed -n 's/.*Saved "file-embedded" WEBP image: \(.*\) ([0-9][0-9]* bytes)\.$/\1/p' "$1" | tail -n 1
}

extract_pin() {
    sed -n 's/.*Recovery PIN: \[\*\*\*\([0-9][0-9]*\)\*\*\*\].*/\1/p' "$1" | tail -n 1
}

extract_recovered_file() {
    sed -n 's/.*Extracted hidden file: \(.*\) ([0-9][0-9]* bytes)\.$/\1/p' "$1" | tail -n 1
}

need_cmd cmp
need_cmd python3
need_cmd sed
need_cmd stat

bash "$TESTS/create_testdata.sh"

if [[ "$NO_BUILD" -eq 0 ]]; then
    (cd "$ROOT" && bash ./compile_wbpdv.sh)
fi
BIN="$(absolute_path "$BIN")"
if [[ ! -x "$BIN" ]]; then
    echo "Binary not found or not executable: $BIN" >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/wbpdv_roundtrip.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

assert_owner_only_permissions() {
    local file="$1" case_id="$2" perms
    perms="$(stat -c '%a' "$file" 2>/dev/null || true)"
    if [[ "$perms" != "600" ]]; then
        echo "[FAIL] $case_id: expected owner-only permissions (600), got ${perms:-unknown}" >&2
        return 1
    fi
    return 0
}

run_case() {
    local case_id="$1" option="$2" payload_rel="$3"
    local input_name="${4:-}"
    local cover="$TESTS/testdata/covers/cover.webp"
    local payload="$TESTS/$payload_rel"
    local work="$WORK/$case_id"

    if [[ -z "$input_name" ]]; then
        input_name="$(basename -- "$payload")"
    fi

    if [[ ! -f "$cover" || ! -f "$payload" ]]; then
        echo "[FAIL] $case_id: missing fixture" >&2
        return 1
    fi

    mkdir -p "$work"
    cp "$cover" "$work/cover.webp"
    cp "$payload" "$work/$input_name"

    pushd "$work" >/dev/null
    if [[ -n "$option" ]]; then
        if ! "$BIN" conceal "$option" cover.webp "$input_name" > conceal.log 2>&1; then
            popd >/dev/null
            echo "[FAIL] $case_id: conceal command failed" >&2
            cat "$work/conceal.log" >&2
            return 1
        fi
    else
        if ! "$BIN" conceal cover.webp "$input_name" > conceal.log 2>&1; then
            popd >/dev/null
            echo "[FAIL] $case_id: conceal command failed" >&2
            cat "$work/conceal.log" >&2
            return 1
        fi
    fi

    local embedded pin
    embedded="$(extract_embedded_image conceal.log)"
    pin="$(extract_pin conceal.log)"
    if [[ -z "$embedded" || -z "$pin" || ! -f "$embedded" ]]; then
        popd >/dev/null
        echo "[FAIL] $case_id: failed to parse conceal output" >&2
        cat "$work/conceal.log" >&2
        return 1
    fi
    if ! assert_owner_only_permissions "$embedded" "$case_id embedded"; then
        popd >/dev/null
        return 1
    fi

    if ! printf '%s\n' "$pin" | "$BIN" recover "$embedded" > recover.log 2>&1; then
        popd >/dev/null
        echo "[FAIL] $case_id: recover command failed" >&2
        cat "$work/recover.log" >&2
        return 1
    fi

    local recovered
    recovered="$(extract_recovered_file recover.log)"
    if [[ -z "$recovered" || ! -f "$recovered" ]]; then
        popd >/dev/null
        echo "[FAIL] $case_id: failed to parse recovered filename" >&2
        cat "$work/recover.log" >&2
        return 1
    fi

    if ! assert_owner_only_permissions "$recovered" "$case_id"; then
        popd >/dev/null
        return 1
    fi

    if ! cmp -s "$recovered" "$payload"; then
        popd >/dev/null
        echo "[FAIL] $case_id: recovered bytes differ from payload" >&2
        return 1
    fi

    popd >/dev/null
    echo "[PASS] $case_id"
    return 0
}

run_cli_argument_checks() {
    local work="$WORK/cli_arguments"
    mkdir -p "$work"
    if ! "$BIN" --info > "$work/info.log" 2>&1; then
        echo "[FAIL] cli_arguments: --info was rejected" >&2
        return 1
    fi
    if "$BIN" --info unexpected > "$work/info-extra.log" 2>&1; then
        echo "[FAIL] cli_arguments: --info accepted a trailing argument" >&2
        return 1
    fi
    echo "[PASS] cli_arguments"
}

run_decode_invalid_cover_check() {
    local work="$WORK/decode_invalid_cover"
    local cover="$TESTS/testdata/covers/cover.webp"
    local payload="$TESTS/testdata/payloads/payload_text.txt"
    mkdir -p "$work"

    # Preserve just enough of this fixture's VP8 payload for WebPGetFeatures()
    # to report 96x96 successfully, while a full WebPDecodeRGBA() fails. The
    # RIFF and VP8 sizes are rewritten so this is a structurally complete but
    # decode-truncated container rather than a mere short read.
    python3 - "$cover" "$work/truncated.webp" <<'PY'
import struct
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_bytes()
if source[:4] != b"RIFF" or source[8:16] != b"WEBPVP8 ":
    raise SystemExit("unexpected cover fixture layout")
chunk_size = struct.unpack_from("<I", source, 16)[0]
payload = source[20:20 + chunk_size]
keep = 227
if len(payload) <= keep:
    raise SystemExit("cover fixture is too small for truncation test")
chunk = b"VP8 " + struct.pack("<I", keep) + payload[:keep] + b"\x00"
result = b"RIFF" + struct.pack("<I", 4 + len(chunk)) + b"WEBP" + chunk
Path(sys.argv[2]).write_bytes(result)
PY

    cp "$payload" "$work/payload.txt"
    if (cd "$work" && "$BIN" conceal truncated.webp payload.txt > conceal.log 2>&1); then
        echo "[FAIL] decode_invalid_cover: feature-readable truncated VP8 was accepted" >&2
        return 1
    fi
    echo "[PASS] decode_invalid_cover"
}

PASS=0
FAIL=0

# case_id  option(.|-b)  payload_rel
CASES=(
    $'default\t.\ttestdata/payloads/payload_text.txt'
    $'filename_spaces\t.\ttestdata/payloads/payload_text.txt\tpayload space.txt'
    $'bluesky\t-b\ttestdata/payloads/payload_text.txt'
    $'bluesky_xmp\t-b\ttestdata/payloads/payload_bsky.bin'
)

for row in "${CASES[@]}"; do
    IFS=$'\t' read -r case_id option payload_rel input_name <<<"$row"
    [[ "$option" == "." ]] && option=""
    if run_case "$case_id" "$option" "$payload_rel" "$input_name"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
done

for check in run_cli_argument_checks run_decode_invalid_cover_check; do
    if "$check"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
done

echo
echo "Round-trip test summary: PASS=$PASS FAIL=$FAIL"
echo "Binary: $BIN"

if [[ "$FAIL" -ne 0 ]]; then
    exit 1
fi
