#!/bin/bash
# Quick redeploy — install rebuilt binaries and restart engine
# Run after: cd /tmp/pi-audio-build && make -j$(nproc)
set -e

BUILD_DIR="/tmp/pi-audio-build"
DSP_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Installing binaries from $BUILD_DIR..."
sudo cp "$BUILD_DIR/pi-audio-engine" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-apply" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-status" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-measure" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-test" /usr/local/bin/
sudo chmod 755 /usr/local/bin/pi-audio-*

# Install systemd service and VM override
sudo cp "$DSP_DIR/pi-audio-engine.service" /etc/systemd/system/
sudo mkdir -p /etc/systemd/system/pi-audio-engine.service.d
sudo cp "$DSP_DIR/overrides/10-vm-user.conf" /etc/systemd/system/pi-audio-engine.service.d/
sudo systemctl daemon-reload

# Fix permissions for runtime directories
sudo chown root:audio /run/pi-audio
sudo chmod 775 /run/pi-audio

# Restart engine
sudo systemctl restart pi-audio-engine
sleep 1

echo ""
echo "Status files:"
ls -la /run/pi-audio/
echo ""
echo "Engine state:"
cat /run/pi-audio/engine-state.json 2>/dev/null || echo "(not yet written)"
echo ""
echo "Status adapter:"
/usr/local/bin/pi-audio-status 2>&1
echo ""
echo "Engine status:"
sudo systemctl is-active pi-audio-engine
