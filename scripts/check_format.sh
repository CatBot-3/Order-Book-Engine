#!/usr/bin/env bash
# Check (default) or fix (--fix) formatting of every C++ file in the repo.
#
#   scripts/check_format.sh [clang-format-binary] [--fix]
set -euo pipefail

BIN="clang-format"
MODE=(--dry-run --Werror)
for arg in "$@"; do
    case "$arg" in
        --fix) MODE=(-i) ;;
        *) BIN="$arg" ;;
    esac
done

cd "$(dirname "$0")/.."
mapfile -t files < <(find include apps bench tests fuzz -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)
if [[ "${#files[@]}" -eq 0 ]]; then
    echo "no source files found"
    exit 0
fi
"$BIN" --version
"$BIN" "${MODE[@]}" "${files[@]}"
echo "formatting ok (${#files[@]} files)"
