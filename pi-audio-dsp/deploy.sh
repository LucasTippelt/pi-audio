#!/bin/bash
# Deployment script for Pi Audio DSP Engine (monorepo)
# Run from: /var/www/pi-audio/

set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DSP_DIR="$REPO_DIR/pi-audio-dsp"
WEB_DIR="$REPO_DIR/pi-audio-web"

echo "Deploying from $REPO_DIR"

# Build DSP engine
echo "Building DSP engine..."
cd "$DSP_DIR"
rm -rf build
mkdir build && cd build
cmake ..
make -j$(nproc)
cd "$REPO_DIR"

# Create directories
sudo mkdir -p /etc/pi-audio/config
sudo mkdir -p /etc/pi-audio/presets
sudo mkdir -p /var/log/pi-audio
sudo mkdir -p /run/pi-audio
sudo chown pi:audio /run/pi-audio

# Install binaries
sudo cp "$DSP_DIR/build/pi-audio-engine" /usr/local/bin/
sudo cp "$DSP_DIR/build/pi-audio-apply" /usr/local/bin/
sudo cp "$DSP_DIR/build/pi-audio-status" /usr/local/bin/
sudo cp "$DSP_DIR/build/pi-audio-measure" /usr/local/bin/
sudo cp "$DSP_DIR/build/pi-audio-test" /usr/local/bin/

# Set permissions
sudo chmod 755 /usr/local/bin/pi-audio-*

# Install systemd service
sudo cp "$DSP_DIR/pi-audio-engine.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable pi-audio-engine

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

echo "Installation complete!"
echo "Start the engine with: sudo systemctl start pi-audio-engine"
echo "Web interface: http://<your-pi-ip>/"
