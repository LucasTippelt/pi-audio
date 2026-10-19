#!/bin/bash
# Setup mock adapters for testing pi-audio-web-rewrite without DSP binaries
# Run: bash setup-mocks.sh [--install]

MOCK_DIR="$(cd "$(dirname "$0")" && pwd)/mock-adapters"
INSTALL_DIR="${1:-/tmp/pi-audio-test-bin}"

echo "Setting up mock adapters..."
echo "Mock directory: $MOCK_DIR"
echo "Install directory: $INSTALL_DIR"

# Create install directory
mkdir -p "$INSTALL_DIR"

# Copy and make executable
for f in "$MOCK_DIR"/pi-audio-*; do
    if [ -f "$f" ]; then
        cp "$f" "$INSTALL_DIR/"
        chmod +x "$INSTALL_DIR/$(basename "$f")"
        echo "Installed: $(basename "$f")"
    fi
done

# Create test directories
mkdir -p /tmp/pi-audio-test-config
mkdir -p /tmp/pi-audio-test-presets
mkdir -p /run/pi-audio

echo ""
echo "Mock adapters installed to $INSTALL_DIR"
echo ""
echo "To use with tests, set environment variables:"
echo "  export PI_AUDIO_STATUS_BIN=$INSTALL_DIR/pi-audio-status"
echo "  export PI_AUDIO_APPLY_BIN=$INSTALL_DIR/pi-audio-apply"
echo "  export PI_AUDIO_TEST_BIN_TWEETER=$INSTALL_DIR/pi-audio-test"
echo "  export PI_AUDIO_CONFIG_DIR=/tmp/pi-audio-test-config"
echo "  export PI_AUDIO_PRESETS_DIR=/tmp/pi-audio-test-presets"
