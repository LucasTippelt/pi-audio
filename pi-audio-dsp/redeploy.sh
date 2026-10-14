#!/bin/bash
# Quick redeploy — install rebuilt binaries and restart all services
# Run after: cd /tmp/pi-audio-build && make -j$(nproc)
set -e

DSP_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$DSP_DIR/build"

echo "Stopping engine..."
sudo systemctl stop pi-audio-engine
sleep 1

echo "Installing binaries from $BUILD_DIR..."
sudo cp "$BUILD_DIR/pi-audio-engine" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-apply" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-status" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-measure" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-test" /usr/local/bin/
sudo chmod 755 /usr/local/bin/pi-audio-*

# Install systemd service
sudo cp "$DSP_DIR/pi-audio-engine.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo mkdir -p /etc/pi-audio/config
sudo mkdir -p /etc/pi-audio/presets
sudo mkdir -p /var/log/pi-audio
sudo mkdir -p /run/pi-audio
sudo chown pi:audio /run/pi-audio
sudo chmod 775 /run/pi-audio

# Restart all services
echo "Restarting services..."
sudo systemctl restart pi-audio-engine
sudo systemctl restart apache2
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
echo "Service status:"
echo "  Engine:       $(sudo systemctl is-active pi-audio-engine)"
echo "  Apache:       $(sudo systemctl is-active apache2)"
