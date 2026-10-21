#!/bin/bash
# Deploy pi-audio-engine binary, systemd unit, and restart services
# Stops service first to avoid "Text file busy" error on cp
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SYSTEMD_DIR="$REPO_ROOT/deploy/systemd"
SERVICE_NAME="pi-audio-engine"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

echo "=== Pi Audio DSP Engine Deployment ==="

# Install systemd service file
echo "[1/4] Installing systemd service..."
sudo cp "$SYSTEMD_DIR/${SERVICE_NAME}.service" "$SERVICE_FILE"
# Remove old override drop-in (now merged into main unit)
if [ -d "/etc/systemd/system/${SERVICE_NAME}.service.d" ]; then
    sudo rm -rf "/etc/systemd/system/${SERVICE_NAME}.service.d"
fi
sudo systemctl daemon-reload

# Stop service to avoid "Text file busy" on binary replace
echo "[2/4] Stopping ${SERVICE_NAME}..."
sudo systemctl stop "$SERVICE_NAME" || true
sleep 1

# Deploy new binary
echo "[3/4] Installing binary..."
sudo cp "$SCRIPT_DIR/build/pi-audio-engine" /usr/local/bin/pi-audio-engine

# Start services (engine first, then web)
echo "[4/4] Starting services..."
sudo systemctl start "$SERVICE_NAME"
sleep 2
if sudo systemctl is-active --quiet "$SERVICE_NAME"; then
    echo "    ${SERVICE_NAME} is running."
else
    echo "    FAILED: Service did not start. Check:"
    echo "    journalctl -u ${SERVICE_NAME} -n 30"
    exit 1
fi

# Restart web server so it picks up the fresh runtime directory
if sudo systemctl list-unit-files | grep -q pi-audio-web.service; then
    echo "    Restarting pi-audio-web..."
    sudo systemctl restart pi-audio-web
    sleep 1
    if sudo systemctl is-active --quiet pi-audio-web; then
        echo "    pi-audio-web is running."
    else
        echo "    WARNING: pi-audio-web failed to start. Check:"
        echo "    journalctl -u pi-audio-web -n 20"
    fi
fi

echo ""
echo "=== Deployment complete ==="
