#!/usr/bin/env bash
# Download one Nasdaq TotalView-ITCH 5.0 sample file, verify its md5 and
# optionally decompress it.
#
#   scripts/fetch_data.sh                    # default file, into ./data
#   scripts/fetch_data.sh -f 12302019.NASDAQ_ITCH50.gz -x
#   scripts/fetch_data.sh -l                 # list what the server offers
#
# The data is never committed (see .gitignore). Read Nasdaq's terms before
# redistributing any part of it.
set -euo pipefail

BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
FILE="01302019.NASDAQ_ITCH50.gz"
DEST="data"
EXTRACT=0
LIST=0

usage() {
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    cat <<EOF

Options:
  -f FILE   file name on the server (default: $FILE)
  -d DIR    destination directory (default: $DEST)
  -x        also decompress, keeping the .gz
  -l        list the server directory and exit
  -h        this help
EOF
}

while getopts "f:d:xlh" opt; do
    case "$opt" in
        f) FILE="$OPTARG" ;;
        d) DEST="$OPTARG" ;;
        x) EXTRACT=1 ;;
        l) LIST=1 ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

for tool in curl md5sum; do
    command -v "$tool" >/dev/null || { echo "error: $tool is required" >&2; exit 1; }
done

if [[ "$LIST" -eq 1 ]]; then
    curl -fsSL "$BASE_URL/" | grep -oE '[0-9]{8}\.NASDAQ_ITCH50\.gz(\.md5sum)?' | sort -u
    exit 0
fi

mkdir -p "$DEST"
gz="$DEST/$FILE"
sum="$gz.md5sum"

echo "==> downloading $FILE (several GB; an interrupted download resumes)"
curl -fL --retry 3 -C - -o "$gz" "$BASE_URL/$FILE"

echo "==> downloading checksum"
curl -fsSL --retry 3 -o "$sum" "$BASE_URL/$FILE.md5sum"

# The checksum file's layout is not relied on: take the first 32-hex-digit token.
expected="$(grep -oiE '[0-9a-f]{32}' "$sum" | head -n1 | tr 'A-F' 'a-f' || true)"
if [[ -z "$expected" ]]; then
    echo "error: no md5 found in $sum" >&2
    exit 1
fi

echo "==> verifying md5"
actual="$(md5sum "$gz" | cut -d' ' -f1)"
if [[ "$expected" != "$actual" ]]; then
    echo "error: md5 mismatch for $gz" >&2
    echo "  expected $expected" >&2
    echo "  actual   $actual" >&2
    exit 1
fi
echo "    ok  $actual"

if [[ "$EXTRACT" -eq 1 ]]; then
    raw="${gz%.gz}"
    echo "==> decompressing to $raw"
    gzip -dc "$gz" >"$raw.partial"
    mv "$raw.partial" "$raw"
    ls -l "$raw"
fi

echo "done. Next: build/release/apps/itch_stats ${gz%.gz}"
