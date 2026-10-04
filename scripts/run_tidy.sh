#!/usr/bin/env bash
# Run clang-tidy over every source file, using the compile commands of an
# existing build directory.
#
#   scripts/run_tidy.sh [build dir] [clang-tidy binary]
#
#   CXX=clang++ cmake --preset debug -B build/clang-debug
#   scripts/run_tidy.sh build/clang-debug
#
# Use a build configured with Clang: clang-tidy does not understand every flag
# GCC records in compile_commands.json.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD="${1:-build/clang-debug}"
TIDY="${2:-clang-tidy}"

if [[ ! -f "$BUILD/compile_commands.json" ]]; then
    echo "error: $BUILD/compile_commands.json not found. Configure that build first." >&2
    exit 1
fi

mapfile -t files < <(find apps bench tests fuzz -type f -name '*.cpp' | sort)
"$TIDY" --version | head -n 2
status=0
for f in "${files[@]}"; do
    "$TIDY" -p "$BUILD" --quiet --warnings-as-errors='*' "$f" 2>/dev/null || status=1
done
if [[ "$status" -ne 0 ]]; then
    echo "clang-tidy reported problems" >&2
    exit 1
fi
echo "clang-tidy ok (${#files[@]} files)"
