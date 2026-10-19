#!/bin/bash
# Deploy pi-audio-engine binary and restart service
# Stops service first to avoid "Text file busy" error on cp
set -e

echo "Stopping pi-audio-engine..."
systemctl stop pi-audio-engine || true
sleep 1

echo "Deploying new binary..."
cp /var/www/pi-audio/pi-audio-dsp-rewrite/build/pi-audio-engine /usr/local/bin/pi-audio-engine

echo "Starting pi-audio-engine..."
systemctl start pi-audio-engine
sleep 2

if systemctl is-active --quiet pi-audio-engine; then
    echo "Deployed and restarted successfully."
else
    echo "Failed to start service. Check: journalctl -u pi-audio-engine"
    exit 1
fi
