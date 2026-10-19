#!/bin/bash
# Build the Pi Audio DSP rewrite engine
# Run from: /var/www/pi-audio/pi-audio-dsp-rewrite/
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# --- Parse arguments (non-interactive override) ---
DIAG_LOG=""
for arg in "$@"; do
    case $arg in
        --diag-log|--debug) DIAG_LOG="ON"; shift ;;
        --no-diag-log|--production) DIAG_LOG="OFF"; shift ;;
        --help|-h)
            echo "Usage: ./build.sh [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --diag-log       Enable diagnostic logging (interactive prompt skipped)"
            echo "  --no-diag-log    Disable diagnostic logging (default, interactive prompt skipped)"
            echo "  --help           Show this help"
            echo ""
            exit 0
            ;;
    esac
done

# --- Interactive prompt: arrow keys + Enter to confirm ---
if [ -z "$DIAG_LOG" ] && [ -t 0 ]; then
    selected="OFF"
    while true; do
        echo ""
        echo -e "${BOLD}Diagnostic Logging (file I/O in audio path)${NC}"
        echo ""

        # Show options with highlight based on selection
        if [ "$selected" = "ON" ]; then
            echo -e "  ${GREEN}▸ [1] ON${NC}    — Enable diagnostic logging (writes to /run/pi-audio/diag.log)"
            echo -e "    [2] OFF   — Disable diagnostic logging (production mode, no file I/O in audio path)"
        else
            echo -e "    [1] ON    — Enable diagnostic logging (writes to /run/pi-audio/diag.log)"
            echo -e "  ${GREEN}▸ [2] OFF${NC}   — Disable diagnostic logging (production mode, no file I/O in audio path)"
        fi
        echo ""
        echo -n "  Choice [1/2/Enter for default (OFF)]: "

        # Read single key press with arrow support
        read -rsn1 key
        case "$key" in
            ''|1) selected="ON"; break ;;
            2) selected="OFF"; break ;;
            $'\x1b')
                # Arrow key sequence: ESC [ A/B
                read -rsn2 seq
                if [[ "$seq" == "[A" ]]; then
                    # Up arrow
                    selected="$([ "$selected" = "ON" ] && echo "OFF" || echo "ON")"
                elif [[ "$seq" == "[B" ]]; then
                    # Down arrow
                    selected="$([ "$selected" = "ON" ] && echo "OFF" || echo "ON")"
                fi
                ;;
        esac
    done
    DIAG_LOG="$selected"
fi

# Default to OFF if not set (non-interactive mode)
DIAG_LOG="${DIAG_LOG:-OFF}"

echo ""
echo "============================================"
echo "  Pi Audio DSP Rewrite — Build"
echo "============================================"
echo ""

# --- Step 1: Configure ---
echo -e "${YELLOW}[1/3] Configuring CMake...${NC}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DPI_AUDIO_DIAG_LOG=$DIAG_LOG >/dev/null 2>&1
echo -e "${GREEN}      Done.${NC}"
if [ "$DIAG_LOG" = "ON" ]; then
    echo -e "      ${CYAN}Diagnostic logging: ENABLED${NC}"
else
    echo -e "      Diagnostic logging: DISABLED (production)"
fi

# --- Step 2: Build ---
echo -e "${YELLOW}[2/3] Building...${NC}"
make -j$(nproc) >/dev/null 2>&1
echo -e "${GREEN}      Build complete.${NC}"

# --- Tests ---
echo -e "${YELLOW}[3/3] Running tests...${NC}"
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
if [ "$DIAG_LOG" = "ON" ]; then
    echo -e "${YELLOW}  Diagnostic logging is enabled.${NC}"
fi
echo -e "${GREEN}  Run ./deploy.sh to install and restart.${NC}"
echo -e "${GREEN}============================================${NC}"
