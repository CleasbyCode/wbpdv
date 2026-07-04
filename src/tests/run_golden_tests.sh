#!/bin/bash
# Recover-only golden-file regression tests for wbpdv.
#
# Each golden/ case ships a pre-built embedded WEBP, its recovery PIN, and the
# expected payload bytes. This catches regressions in recover, RIFF/WEBP chunk
# parsing, and the per-mode embed layouts (standard ICCP / Bluesky EXIF+XMP)
# without relying on fresh conceal RNG.
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TESTS/.." && pwd)"
GOLDEN="$TESTS/golden"
MANIFEST="$GOLDEN/manifest.tsv"
BIN="${WBPDV_BIN:-$ROOT/wbpdv}"
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: tests/run_golden_tests.sh [options]

Options:
  --no-build    Reuse existing wbpdv binary.
  --bin <path>  Use an explicit binary path.
  -h, --help    Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build) NO_BUILD=1; shift;;
        --bin) BIN="$2"; NO_BUILD=1; shift 2;;
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

need_cmd python3
need_cmd cmp
need_cmd stat

if [[ "$NO_BUILD" -eq 0 ]]; then
    (cd "$ROOT" && bash ./compile_wbpdv.sh)
fi
if [[ ! -x "$BIN" ]]; then
    echo "Binary not found or not executable: $BIN" >&2
    exit 1
fi
if [[ ! -f "$MANIFEST" ]]; then
    echo "Missing golden manifest: $MANIFEST" >&2
    echo "Generate fixtures with: bash tests/generate_golden.sh" >&2
    exit 1
fi

extract_recovered_file() {
    sed -n 's/.*Extracted hidden file: \([^ ]*\) (.*/\1/p' "$1" | tail -n 1
}

assert_owner_only_permissions() {
    local perms
    perms="$(stat -c '%a' "$1" 2>/dev/null || true)"
    if [[ "$perms" != "600" ]]; then
        echo "[FAIL] $2: expected owner-only permissions (600), got ${perms:-unknown}" >&2
        return 1
    fi
    return 0
}

# Walk the RIFF/WEBP chunks and assert the per-mode embed layout matches `kind`:
#   standard    -> an ICCP chunk carrying the ICC profile (mntrRGB) and the WDV
#                  signature; no EXIF/XMP payload chunks.
#   bluesky     -> an EXIF chunk ("Exif\0\0" + WDV signature) and no XMP chunk.
#   bluesky_xmp -> the EXIF chunk plus an XMP chunk (ciphertext overflow).
assert_webp_structure() {
    local image="$1" kind="$2" tag="$3"
    if ! WBPDV_IMG="$image" WBPDV_KIND="$kind" python3 - <<'PY'
import os, sys
from pathlib import Path

data = Path(os.environ["WBPDV_IMG"]).read_bytes()
kind = os.environ["WBPDV_KIND"]

WDV  = bytes([0xB4, 0x77, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9])
ICC  = b"mntrRGB"
EXIF = b"Exif\x00\x00"

if data[:4] != b"RIFF" or data[8:12] != b"WEBP":
    sys.exit("not a RIFF/WEBP container")

def chunks(d):
    pos = 12
    n = len(d)
    while pos + 8 <= n:
        tag = d[pos:pos + 4]
        size = int.from_bytes(d[pos + 4:pos + 8], "little")
        if pos + 8 + size > n:
            break
        yield tag, d[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)

iccp = exif = xmp = None
for tag, body in chunks(data):
    if tag == b"ICCP" and iccp is None:
        iccp = body
    elif tag == b"EXIF" and exif is None:
        exif = body
    elif tag == b"XMP " and xmp is None:
        xmp = body

if kind == "standard":
    if iccp is None:
        sys.exit("missing ICCP chunk")
    if ICC not in iccp:
        sys.exit("ICCP chunk missing ICC profile signature")
    if WDV not in iccp:
        sys.exit("ICCP chunk missing WDV signature")
    if exif is not None or xmp is not None:
        sys.exit("unexpected EXIF/XMP chunk in standard image")
elif kind in ("bluesky", "bluesky_xmp"):
    if exif is None:
        sys.exit("missing EXIF chunk")
    if exif[:6] != EXIF:
        sys.exit("EXIF chunk missing Exif header")
    if WDV not in exif:
        sys.exit("EXIF chunk missing WDV signature")
    if kind == "bluesky_xmp" and xmp is None:
        sys.exit("missing XMP overflow chunk")
    if kind == "bluesky" and xmp is not None:
        sys.exit("unexpected XMP chunk (payload should fit EXIF)")
else:
    sys.exit(f"unknown kind: {kind}")
PY
    then
        echo "[FAIL] $tag: WEBP structure check failed" >&2
        return 1
    fi
    return 0
}

PASS=0
FAIL=0

run_case() {
    local case_id="$1" option="$2" payload_rel="$3" golden_rel="$4" pin="$5" kind="$6"
    local payload="$TESTS/$payload_rel"
    local golden="$TESTS/$golden_rel"
    local work="$TESTS/.work/$case_id"

    if [[ ! -f "$payload" ]]; then
        echo "[FAIL] $case_id: missing payload $payload_rel" >&2
        return 1
    fi
    if [[ ! -f "$golden" ]]; then
        echo "[FAIL] $case_id: missing golden image $golden_rel (run generate_golden.sh)" >&2
        return 1
    fi

    rm -rf "$work"
    mkdir -p "$work"
    cp "$golden" "$work/input.webp"

    assert_webp_structure "$golden" "$kind" "$case_id" || return 1

    pushd "$work" >/dev/null
    if ! printf '%s\n' "$pin" | "$BIN" recover input.webp > recover.log 2>&1; then
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
        echo "[FAIL] $case_id: recovered bytes differ from golden payload" >&2
        return 1
    fi

    popd >/dev/null
    echo "[PASS] $case_id"
    return 0
}

mkdir -p "$TESTS/.work"
trap 'rm -rf "$TESTS/.work"' EXIT

while IFS=$'\t' read -r case_id option payload_rel golden_rel pin kind; do
    [[ -z "$case_id" || "$case_id" == "case_id" ]] && continue
    [[ "$option" == "." ]] && option=""
    if run_case "$case_id" "$option" "$payload_rel" "$golden_rel" "$pin" "$kind"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
done < "$MANIFEST"

echo
echo "Golden test summary: PASS=$PASS FAIL=$FAIL"
echo "Binary: $BIN"

if [[ "$FAIL" -ne 0 ]]; then
    exit 1
fi
