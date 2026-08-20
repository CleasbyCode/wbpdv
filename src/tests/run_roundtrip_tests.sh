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
need_cmd grep
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
    if ! grep -Fq "Bluesky   (2,000,000 Bytes / ~1.9MB)" "$work/info.log"; then
        echo "[FAIL] cli_arguments: --info has a stale Bluesky size limit" >&2
        return 1
    fi
    echo "[PASS] cli_arguments"
}

run_reencode_notice_check() {
    local work="$WORK/reencode_notice"
    local cover="$TESTS/testdata/covers/cover_lossless_alpha.webp"
    local payload="$TESTS/testdata/payloads/payload_text.txt"
    mkdir -p "$work"
    cp "$cover" "$work/cover.webp"
    cp "$payload" "$work/payload.txt"

    if ! (cd "$work" &&
          "$BIN" conceal -b cover.webp payload.txt > conceal.log 2>&1); then
        echo "[FAIL] reencode_notice: conceal command failed" >&2
        cat "$work/conceal.log" >&2
        return 1
    fi
    if grep -Fq "Cover image re-encoded to lossy WEBP" "$work/conceal.log"; then
        echo "[FAIL] reencode_notice: removed informational message was printed" >&2
        return 1
    fi
    echo "[PASS] reencode_notice"
}

run_bluesky_size_limit_checks() {
    local work="$WORK/bluesky_size_limit"
    local fit="$work/fit"
    local over="$work/over"
    local cover="$TESTS/testdata/covers/cover.webp"
    mkdir -p "$fit" "$over"
    cp "$cover" "$fit/cover.webp"
    cp "$cover" "$over/cover.webp"

    python3 - "$fit/payload.bin" "$over/payload.bin" <<'PY'
import hashlib
import sys
from pathlib import Path

Path(sys.argv[1]).write_bytes(
    hashlib.shake_256(b"wbpdv-limit-fit").digest(850_000)
)
Path(sys.argv[2]).write_bytes(
    hashlib.shake_256(b"wbpdv-limit-over").digest(1_550_000)
)
PY

    if ! (cd "$fit" &&
          "$BIN" conceal -b cover.webp payload.bin > conceal.log 2>&1); then
        echo "[FAIL] bluesky_size_limit: output between old and new limits failed" >&2
        cat "$fit/conceal.log" >&2
        return 1
    fi

    local embedded fit_size
    embedded="$(extract_embedded_image "$fit/conceal.log")"
    if [[ -z "$embedded" || ! -f "$fit/$embedded" ]]; then
        echo "[FAIL] bluesky_size_limit: failed to find successful output" >&2
        cat "$fit/conceal.log" >&2
        return 1
    fi
    fit_size="$(stat -c '%s' "$fit/$embedded")"
    if (( fit_size <= 1000000 || fit_size > 2000000 )); then
        echo "[FAIL] bluesky_size_limit: expected output in (1,000,000, 2,000,000], got $fit_size" >&2
        return 1
    fi

    if (cd "$over" &&
        "$BIN" conceal -b cover.webp payload.bin > conceal.log 2>&1); then
        echo "[FAIL] bluesky_size_limit: output above 2,000,000 bytes succeeded" >&2
        return 1
    fi
    if find "$over" -maxdepth 1 -type f -name 'wbpdv_*.webp' -print -quit |
        grep -q .; then
        echo "[FAIL] bluesky_size_limit: oversized output file was committed" >&2
        return 1
    fi

    # Must stay guarded: this function runs as an `if` condition, which
    # suppresses `set -e` for its whole body, so an unchecked python3 here
    # would let a mismatched message fall through to the [PASS] below.
    if ! python3 - "$over/conceal.log" <<'PY'
import re
import sys
from pathlib import Path

actual = Path(sys.argv[1]).read_text().strip()
expected = (
    r"File Size Error: Output file \((\d+) bytes\) exceeds the Bluesky upload "
    r"limit of 2,000,000 bytes \(~1\.9MB\)\. Try a smaller cover image or "
    r"reduce the size of the payload \(hidden data file\)\."
)
match = re.fullmatch(expected, actual)
if match is None:
    raise SystemExit(f"unexpected Bluesky limit error: {actual!r}")
if int(match.group(1)) <= 2_000_000:
    raise SystemExit(f"reported oversized byte count is not above limit: {actual!r}")
PY
    then
        echo "[FAIL] bluesky_size_limit: unexpected over-limit error message" >&2
        return 1
    fi

    echo "[PASS] bluesky_size_limit"
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

run_pin_suspend_resume_check() {
    local work="$WORK/pin_suspend_resume"
    mkdir -p "$work"

    # Ctrl-Z during PIN entry must re-prompt on resume. Before this was handled,
    # raise(SIGTSTP) returned after `fg` and getPin() fell through to its
    # failure sentinel, so a suspend/resume was reported as "Invalid PIN or file
    # is corrupt". wbpdv runs as a grandchild in its own foreground process
    # group: a session leader's group is orphaned, and the kernel discards stop
    # signals there, which would skip the path under test entirely.
    if ! python3 - "$BIN" "$TESTS/testdata/covers/cover.webp" "$work" <<'PY'
import os, pty, re, select, signal, subprocess, sys, time
from pathlib import Path

binary, cover, work = sys.argv[1], sys.argv[2], Path(sys.argv[3])
payload = b"suspend-resume regression payload\n"
(work / "cover.webp").write_bytes(Path(cover).read_bytes())
(work / "secret.txt").write_bytes(payload)
os.chdir(work)

out = subprocess.run([binary, "conceal", "cover.webp", "secret.txt"],
                     capture_output=True, text=True, check=True).stdout
pin = re.search(r"Recovery PIN: \[\*\*\*(\d+)\*\*\*\]", out).group(1)
img = re.search(r'Saved "file-embedded" WEBP image: (\S+) \(', out).group(1)
(work / "secret.txt").unlink()

pid, fd = pty.fork()
if pid == 0:
    grand = os.fork()
    if grand == 0:
        try:
            os.setpgid(0, 0)
            # tcsetpgrp from a background group raises SIGTTOU, which would stop
            # us before exec. SIG_IGN survives execv, so restore SIG_DFL after.
            signal.signal(signal.SIGTTOU, signal.SIG_IGN)
            os.tcsetpgrp(0, os.getpgrp())
            signal.signal(signal.SIGTTOU, signal.SIG_DFL)
            os.execv(binary, [binary, "recover", img])
        except BaseException as exc:
            os.write(2, f"job setup failed: {exc!r}\n".encode())
            os._exit(127)
    while True:  # keep the job's process group from being orphaned
        _, status = os.waitpid(grand, os.WUNTRACED)
        if os.WIFSTOPPED(status):
            continue
        os._exit(os.WEXITSTATUS(status) if os.WIFEXITED(status) else 1)

transcript = b""

def pump(seconds):
    global transcript
    end = time.time() + seconds
    while time.time() < end:
        if not select.select([fd], [], [], max(0.0, end - time.time()))[0]:
            return
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            return
        if not chunk:
            return
        transcript += chunk

def state(p):
    try:
        return Path(f"/proc/{p}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except (OSError, IndexError):
        return "?"

def fail(message):
    raise SystemExit(f"{message}\ntranscript: {transcript!r}")

pump(5.0)
if b"PIN:" not in transcript:
    fail("no PIN prompt")
before = transcript.count(b"PIN:")

job = os.tcgetpgrp(fd)
os.write(fd, b"\x1a")  # a real Ctrl-Z through the line discipline
deadline = time.time() + 10
while time.time() < deadline and state(job) != "T":
    pump(0.1)
if state(job) != "T":
    fail(f"Ctrl-Z did not suspend the job (state {state(job)})")

os.killpg(job, signal.SIGCONT)  # what `fg` does
pump(5.0)
if transcript.count(b"PIN:") <= before:
    fail("PIN was not re-prompted after resume")

os.write(fd, (pin + "\n").encode())
pump(10.0)
_, status = os.waitpid(pid, 0)
code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -1
text = transcript.decode(errors="replace")
if code != 0:
    fail(f"recover exited {code}")
if "Invalid PIN" in text:
    fail("resume was reported as an invalid PIN")
if "Extracted hidden file" not in text:
    fail("no file was extracted")
if (work / "secret.txt").read_bytes() != payload:
    fail("recovered payload differs")
PY
    then
        echo "[FAIL] pin_suspend_resume: Ctrl-Z during PIN entry did not resume cleanly" >&2
        return 1
    fi
    echo "[PASS] pin_suspend_resume"
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

for check in run_cli_argument_checks run_reencode_notice_check \
             run_bluesky_size_limit_checks run_decode_invalid_cover_check \
             run_pin_suspend_resume_check; do
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
