#!/bin/bash
# Generate version info for all components
# Version format: vX.Y.Z.UIO
#   X = major release (main branch)
#   Y = staging release
#   Z = local build
#   UIO = debug iteration (from VERSION file or 0)
# Run from repo root: bash scripts/gen-version.sh [BUILD_DIR]
set -e

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${1:-$REPO_DIR/pi-audio-dsp/build}"
mkdir -p "$BUILD_DIR"

# Read base version (X.Y.Z) from VERSION file
BASE_VER="0.0.0"
if [ -f "$REPO_DIR/VERSION" ]; then
    BASE_VER=$(head -1 "$REPO_DIR/VERSION" | tr -d '[:space:]')
fi

# Read debug iteration (UIO) from VERSION file, default 0, then increment
UIO=0
if [ -f "$REPO_DIR/VERSION" ]; then
    line2=$(sed -n '2p' "$REPO_DIR/VERSION" | tr -d '[:space:]')
    if [[ "$line2" =~ ^[0-9]+$ ]]; then
        UIO=$((line2 + 1))
    fi
fi

# Write incremented UIO back to VERSION file (line 2)
{
    head -1 "$REPO_DIR/VERSION"
    echo "$UIO"
} > "$REPO_DIR/VERSION.tmp"
mv "$REPO_DIR/VERSION.tmp" "$REPO_DIR/VERSION"

GIT_HASH=$(cd "$REPO_DIR" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_BRANCH=$(cd "$REPO_DIR" && git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
GIT_DIRTY=$(cd "$REPO_DIR" && git diff --quiet 2>/dev/null || echo "-dirty")
BUILD_TIME=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

VERSION="v${BASE_VER}.${UIO}"

# Write VERSION.json (readable by PHP, Node.js, etc.)
cat > "$REPO_DIR/pi-audio-web/VERSION.json" <<EOF
{
  "version": "$VERSION",
  "base": "$BASE_VER",
  "commit": "$GIT_HASH",
  "branch": "$GIT_BRANCH",
  "buildTime": "$BUILD_TIME",
  "dirty": $([ -n "$GIT_DIRTY" ] && echo "true" || echo "false")
}
EOF

# Write C++ version header
cat > "$BUILD_DIR/version_generated.h" <<EOF
#ifndef PI_AUDIO_VERSION_GENERATED_H
#define PI_AUDIO_VERSION_GENERATED_H

#define PI_AUDIO_VERSION "$VERSION"
#define PI_AUDIO_COMMIT "$GIT_HASH"
#define PI_AUDIO_BRANCH "$GIT_BRANCH"
#define PI_AUDIO_BUILD_TIME "$BUILD_TIME"

namespace pi_audio {
inline const char* getVersion() { return PI_AUDIO_VERSION; }
inline const char* getCommit() { return PI_AUDIO_COMMIT; }
inline const char* getBranch() { return PI_AUDIO_BRANCH; }
inline const char* getBuildTime() { return PI_AUDIO_BUILD_TIME; }
}

#endif
EOF

echo "Version: $VERSION ($GIT_HASH on $GIT_BRANCH, built $BUILD_TIME)"
