#!/bin/bash
# Build wbpdv and regenerate the committed golden embedded-WEBP fixtures and
# manifest. Re-run after an intentional format/crypto-layout change, then review
# the diffs to golden/*/embedded.webp and golden/manifest.tsv before committing.
# Encryption nonces, salts, and PINs intentionally remain random, so byte-level
# fixture and manifest diffs are expected. Generation is staged completely
# before the old corpus is replaced; production randomness is never weakened.
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TESTS/.." && pwd)"
GOLDEN="$TESTS/golden"
MANIFEST="$GOLDEN/manifest.tsv"
BIN="${WBPDV_BIN:-$ROOT/wbpdv}"
NO_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build) NO_BUILD=1; shift;;
        *) echo "Unknown option: $1" >&2; exit 2;;
    esac
done

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

bash "$TESTS/create_testdata.sh"

if [[ "$NO_BUILD" -eq 0 ]]; then
    (cd "$ROOT" && bash ./compile_wbpdv.sh)
fi
BIN="$(absolute_path "$BIN")"
if [[ ! -x "$BIN" ]]; then
    echo "Binary not found: $BIN" >&2
    exit 1
fi

extract_embedded_image() {
    sed -n 's/.*Saved "file-embedded" WEBP image: \(.*\) ([0-9][0-9]* bytes)\.$/\1/p' "$1" | tail -n 1
}
extract_pin() {
    sed -n 's/.*Recovery PIN: \[\*\*\*\([0-9][0-9]*\)\*\*\*\].*/\1/p' "$1" | tail -n 1
}

# case_id  option(.|-b)  payload_rel  kind(standard|bluesky|bluesky_xmp)
# "." means the default (no) conceal option.
CASES=(
    $'standard_text\t.\ttestdata/payloads/payload_text.txt\tstandard'
    $'standard_bin\t.\ttestdata/payloads/payload_bin.bin\tstandard'
    $'bluesky_text\t-b\ttestdata/payloads/payload_text.txt\tbluesky'
    $'bluesky_xmp\t-b\ttestdata/payloads/payload_bsky.bin\tbluesky_xmp'
)

COVER_REL="testdata/covers/cover.webp"

TXN_ROOT="$(mktemp -d "$TESTS/.golden-txn.XXXXXX")"
STAGED_GOLDEN="$TXN_ROOT/new"
WORK_ROOT="$TXN_ROOT/work"
mkdir -p "$STAGED_GOLDEN" "$WORK_ROOT"

cleanup() {
    local status=$?
    local preserve_transaction=0
    trap - EXIT
    # If installation was interrupted after moving the old corpus aside but
    # before publishing the staged one, restore it before cleaning the txn.
    if [[ -d "$TXN_ROOT/old" && ! -e "$GOLDEN" ]]; then
        if ! mv -- "$TXN_ROOT/old" "$GOLDEN"; then
            echo "Error: failed to restore previous golden corpus from $TXN_ROOT/old" >&2
            status=1
        fi
    elif [[ -d "$TXN_ROOT/old" && "$status" -ne 0 ]]; then
        echo "Error: previous golden corpus preserved at $TXN_ROOT/old" >&2
        preserve_transaction=1
        status=1
    fi
    if [[ "$preserve_transaction" -eq 0 ]]; then
        rm -rf -- "$TXN_ROOT"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

{
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        case_id option payload_rel golden_rel pin kind
    for row in "${CASES[@]}"; do
        IFS=$'\t' read -r case_id option payload_rel kind <<<"$row"
        opt="$option"
        [[ "$opt" == "." ]] && opt=""

        cover="$TESTS/$COVER_REL"
        payload="$TESTS/$payload_rel"
        if [[ ! -f "$cover" || ! -f "$payload" ]]; then
            echo "Missing fixture for $case_id" >&2
            exit 1
        fi

        mkdir -p "$STAGED_GOLDEN/$case_id"
        work="$WORK_ROOT/$case_id"
        mkdir "$work"
        pushd "$work" >/dev/null
        cp "$cover" cover.webp
        cp "$payload" "$(basename "$payload")"
        if [[ -n "$opt" ]]; then
            "$BIN" conceal "$opt" cover.webp "$(basename "$payload")" > conceal.log 2>&1
        else
            "$BIN" conceal cover.webp "$(basename "$payload")" > conceal.log 2>&1
        fi
        embedded="$(extract_embedded_image conceal.log)"
        pin="$(extract_pin conceal.log)"
        if [[ -z "$embedded" || -z "$pin" || ! -f "$embedded" ]]; then
            echo "Conceal failed for $case_id:" >&2
            cat conceal.log >&2
            popd >/dev/null
            exit 1
        fi
        popd >/dev/null

        golden_rel="golden/$case_id/embedded.webp"
        cp "$work/$embedded" "$STAGED_GOLDEN/$case_id/embedded.webp"

        printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$case_id" "$option" "$payload_rel" "$golden_rel" "$pin" "$kind"
    done
} > "$STAGED_GOLDEN/manifest.tsv"

# Publish only after every new fixture and the complete manifest exist. Both
# renames remain on the tests filesystem. The EXIT trap rolls back the first
# rename if the second one fails or the script is interrupted between them.
if [[ -e "$GOLDEN" ]]; then
    mv -- "$GOLDEN" "$TXN_ROOT/old"
fi
mv -- "$STAGED_GOLDEN" "$GOLDEN"

echo "Golden fixtures written to: $GOLDEN"
echo "Manifest: $MANIFEST"
wc -l "$MANIFEST"
