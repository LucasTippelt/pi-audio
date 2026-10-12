#!/bin/bash
# VM deployment script for Pi Audio DSP Engine
# Adapted for VM environment — uses current user instead of pi:audio

set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DSP_DIR="$REPO_DIR/pi-audio-dsp"
BUILD_DIR="/tmp/pi-audio-build"

echo "Deploying from $REPO_DIR (VM mode)"

# Verify binaries exist in temp build dir
if [ ! -f "$BUILD_DIR/pi-audio-engine" ]; then
    echo "ERROR: Pre-built binaries not found in $BUILD_DIR"
    echo "Run the build first: cd /tmp/pi-audio-build && cmake $DSP_DIR && make -j\$(nproc)"
    exit 1
fi

# Create directories
sudo mkdir -p /etc/pi-audio/config
sudo mkdir -p /etc/pi-audio/presets
sudo mkdir -p /var/log/pi-audio
sudo mkdir -p /run/pi-audio

# Install binaries
sudo cp "$BUILD_DIR/pi-audio-engine" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-apply" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-status" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-measure" /usr/local/bin/
sudo cp "$BUILD_DIR/pi-audio-test" /usr/local/bin/

# Set permissions
sudo chmod 755 /usr/local/bin/pi-audio-*

# Install systemd service
sudo cp "$DSP_DIR/pi-audio-engine.service" /etc/systemd/system/

# Install VM override (uses current user instead of pi:audio)
sudo mkdir -p /etc/systemd/system/pi-audio-engine.service.d
sudo cp "$DSP_DIR/overrides/10-vm-user.conf" /etc/systemd/system/pi-audio-engine.service.d/

sudo systemctl daemon-reload
sudo systemctl enable pi-audio-engine

# Set ownership to current user instead of pi:audio
CURRENT_USER=$(whoami)
sudo chown -R "$CURRENT_USER":"$CURRENT_USER" /etc/pi-audio/
sudo chmod -R 775 /etc/pi-audio/

# Create default configuration if it doesn't exist
if [ ! -f /etc/pi-audio/config/current.json ]; then
    cat > /tmp/current.json << 'EOF'
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
    sudo cp /tmp/current.json /etc/pi-audio/config/current.json
    sudo chown "$CURRENT_USER":"$CURRENT_USER" /etc/pi-audio/config/current.json
    rm /tmp/current.json
fi

# Install Apache virtual host config
sudo tee /etc/apache2/sites-enabled/pi-audio.conf > /dev/null << 'EOF'
<VirtualHost *:80>
    DocumentRoot /var/www/pi-audio/pi-audio-web
    <Directory /var/www/pi-audio/pi-audio-web>
        AllowOverride All
        Require all granted
    </Directory>
    SetEnv PI_AUDIO_STATUS_BIN /usr/local/bin/pi-audio-status
    SetEnv PI_AUDIO_APPLY_BIN /usr/local/bin/pi-audio-apply
    SetEnv PI_AUDIO_MEASURE_BIN /usr/local/bin/pi-audio-measure
    SetEnv PI_AUDIO_TEST_BIN_WOOFER_L /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_WOOFER_R /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_TWEETER_L /usr/local/bin/pi-audio-test
    SetEnv PI_AUDIO_TEST_BIN_TWEETER_R /usr/local/bin/pi-audio-test
</VirtualHost>
EOF

sudo systemctl restart apache2

echo "VM Deployment complete!"
echo "Start the engine with: sudo systemctl start pi-audio-engine"
echo "Check status with: sudo systemctl status pi-audio-engine"
