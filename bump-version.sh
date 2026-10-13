#!/bin/bash
# bump-version.sh - Bump minor version (Z) and reset build counter (UIO) to 0
# Usage: ./bump-version.sh [new_z_value]
#   If no argument given, increments current Z by 1

REPO_DIR="$(git rev-parse --show-toplevel)"
VERSION_FILE="$REPO_DIR/VERSION"

[ -f "$VERSION_FILE" ] || { echo "VERSION file not found"; exit 1; }

base_ver=$(head -1 "$VERSION_FILE" | tr -d '[:space:]')
X=$(echo "$base_ver" | cut -d. -f1)
Y=$(echo "$base_ver" | cut -d. -f2)
Z=$(echo "$base_ver" | cut -d. -f3)

if [ -n "$1" ]; then
    Z="$1"
else
    Z=$((Z + 1))
fi

{
    echo "${X}.${Y}.${Z}"
    echo "0"
} > "$VERSION_FILE"

echo "Set version to ${X}.${Y}.${Z}.0"
