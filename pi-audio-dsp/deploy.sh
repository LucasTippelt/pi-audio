#!/bin/bash
# Deployment script for Pi Audio DSP Engine (monorepo)
# Run from: /var/www/pi-audio/pi-audio-dsp/
set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DSP_DIR="$REPO_DIR/pi-audio-dsp"
WEB_DIR="$REPO_DIR/pi-audio-web"
BUILD_DIR="$DSP_DIR/build"

echo "Deploying from $REPO_DIR"

# Build DSP engine
echo "Building DSP engine..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake "$DSP_DIR"
make -j$(nproc)
cd "$REPO_DIR"

# Create directories
sudo mkdir -p /etc/pi-audio/config
sudo mkdir -p /etc/pi-audio/presets
sudo mkdir -p /var/log/pi-audio
sudo mkdir -p /run/pi-audio
sudo chown pi:audio /run/pi-audio

# Set group to audio so www-data (in audio group) can read/write
sudo chown -R pi:audio /etc/pi-audio/
sudo chmod -R 775 /etc/pi-audio/

# Create default configuration if it doesn't exist
if [ ! -f /etc/pi-audio/config/current.json ]; then
    cat > /etc/pi-audio/config/current.json << 'EOF'
{
  "input": "default",
  "output": "default",
  "sampleRate": 48000,
  "BufferSize": 512,
  "volume": 65,
  "muted": false,
  "crossovers": {
    "Woofer_L": {"frequency": 80, "topology": "Linkwitz-Riley", "slope": 24},
    "Tweeter_L": {"frequency": 80, "topology": "Linkwitz-Riley", "slope": 24},
    "Woofer_R": {"frequency": 80, "topology": "Linkwitz-Riley", "slope": 24},
    "Tweeter_R": {"frequency": 80, "topology": "Linkwitz-Riley", "slope": 24}
  },
  "bands": {
    "Woofer_L": [],
    "Tweeter_L": [],
    "Woofer_R": [],
    "Tweeter_R": []
  },
  "drivers": [
    {"name": "Woofer_L", "delayMs": 0, "trimDb": 0, "phaseInverted": false},
    {"name": "Tweeter_L", "delayMs": 0, "trimDb": 0, "phaseInverted": false},
    {"name": "Woofer_R", "delayMs": 0, "trimDb": 0, "phaseInverted": false},
    {"name": "Tweeter_R", "delayMs": 0, "trimDb": 0, "phaseInverted": false}
  ],
  "limiter": {
    "enabled": true,
    "threshold": -1,
    "release": 80
  }
}
EOF
    sudo chown pi:audio /etc/pi-audio/config/current.json
fi

# Install WebSocket server service
sudo cp "$REPO_DIR/pi-audio-ws-server.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable pi-audio-ws-server

# Call redeploy.sh to install binaries and restart all services
echo ""
echo "Running redeploy..."
bash "$DSP_DIR/redeploy.sh"

echo ""
echo "Installation complete!"
echo "Web interface: http://<your-pi-ip>/"
