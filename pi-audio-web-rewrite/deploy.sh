#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVICE_NAME="pi-audio-web"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
SYSTEMD_DIR="$REPO_ROOT/deploy/systemd"

echo "=== Pi Audio Web Rewrite Deployment ==="

# Build TypeScript
echo "[1/5] Building TypeScript..."
cd "$PROJECT_DIR"
npm ci --prefer-offline 2>/dev/null || npm install
npx tsc

# Copy frontend assets to dist/ (tsc only compiles .ts files)
cp -r src/frontend dist/

# Install systemd service
echo "[2/5] Installing systemd service..."
sudo cp "$SYSTEMD_DIR/${SERVICE_NAME}.service" "$SERVICE_FILE"
sudo systemctl daemon-reload

# Stop old PHP web server if running
if sudo systemctl is-active --quiet pi-audio-web-old 2>/dev/null; then
  echo "    Stopping legacy pi-audio-web..."
  sudo systemctl stop pi-audio-web-old || true
fi

# Disable Apache (serves old PHP interface on port 80)
echo "[3/5] Disabling Apache..."
sudo systemctl stop apache2 || true
sudo systemctl disable apache2 || true
sudo a2dissite pi-audio.conf || true

# Set CAP_NET_BIND_SERVICE on Node.js binary for port 80 binding
echo "    Setting capabilities on Node.js..."
sudo setcap 'cap_net_bind_service=+ep' "$(which node)"

# Ensure engine service is running (web requires it)
if ! sudo systemctl is-active --quiet pi-audio-engine; then
  echo "[3.5/5] Starting pi-audio-engine (required dependency)..."
  sudo systemctl start pi-audio-engine
  sleep 2
fi

# Restart new service
echo "[4/5] Starting ${SERVICE_NAME}..."
sudo systemctl restart "$SERVICE_NAME"

# Verify
echo "[5/5] Verifying service..."
sleep 1
if sudo systemctl is-active --quiet "$SERVICE_NAME"; then
  echo "    Service is running."
  curl -s http://localhost:80/health || true
else
  echo "    WARNING: Service failed to start. Check journal:"
  echo "    sudo journalctl -u ${SERVICE_NAME} -n 20"
  exit 1
fi

echo ""
echo "=== Deployment complete ==="
echo "Web UI: http://<pi-ip>"
echo "Audio WS: ws://<pi-ip>/ws?ch=pgm-l"
