#!/bin/bash
# Build the Pi Audio DSP rewrite engine
# Run from: /var/www/pi-audio/pi-audio-dsp-rewrite/
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "============================================"
echo "  Pi Audio DSP Rewrite — Build"
echo "============================================"
echo ""

# --- Step 1: Configure ---
echo -e "${YELLOW}[1/2] Configuring CMake...${NC}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
echo -e "${GREEN}      Done.${NC}"

# --- Step 2: Build ---
echo -e "${YELLOW}[2/2] Building...${NC}"
make -j$(nproc) >/dev/null 2>&1
echo -e "${GREEN}      Build complete.${NC}"
echo ""

# --- Tests ---
echo -e "${YELLOW}Running tests...${NC}"
ctest --output-on-failure 2>&1 | grep -E "Test #|passed|failed|^$" || true
TEST_RESULT=${PIPESTATUS[0]}

if [ $TEST_RESULT -ne 0 ]; then
    echo -e "${RED}      Tests failed.${NC}"
    exit 1
fi
echo -e "${GREEN}      All tests passed.${NC}"
echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Build successful — ready to deploy.${NC}"
echo -e "${GREEN}  Run ./deploy.sh to install and restart.${NC}"
echo -e "${GREEN}============================================${NC}"
