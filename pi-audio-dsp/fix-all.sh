#!/bin/bash
# Fix all permissions for VM environment
set -e

echo "=== Fixing directory permissions ==="

# Fix /run/pi-audio - engine needs to write status files
sudo chown -R lucas:audio /run/pi-audio
sudo chmod 775 /run/pi-audio

# Fix /var/log/pi-audio - engine needs to write log files
sudo chown -R lucas:audio /var/log/pi-audio
sudo chmod 775 /var/log/pi-audio

# Fix config files - engine needs to read, webapp needs to write
sudo chmod 664 /etc/pi-audio/config/current.json
sudo find /etc/pi-audio/presets -name "*.json" -exec sudo chmod 664 {} \;

# Fix api.php - write with 664 instead of 660
sudo sed -i 's/@chmod($path, 0660);/@chmod($path, 0664);/' /var/www/pi-audio/pi-audio-web/api.php

echo ""
echo "=== Fixing console logging in app.js ==="

# Check if console logging already exists
if ! grep -q 'console.error.*\[pi-audio\].*API error' /var/www/pi-audio/pi-audio-web/assets/app.js; then
    # Add console.error before "throw error;" in the request() function
    sudo sed -i '/^    throw error;$/{i\    console.error("[pi-audio] API error (" + action + "):", data.error || response.status, data);
}' /var/www/pi-audio/pi-audio-web/assets/app.js
    echo "Added console.error to request() function"
else
    echo "Console logging already present in request()"
fi

# Add console.log for DSP status
if ! grep -q 'console.log.*DSP status response' /var/www/pi-audio/pi-audio-web/assets/app.js; then
    sudo sed -i "/const data = await request('dsp-status');/a\\      console.log('[pi-audio] DSP status response:', data);" /var/www/pi-audio/pi-audio-web/assets/app.js
    echo "Added console.log for DSP status"
else
    echo "DSP status logging already present"
fi

echo ""
echo "=== Restarting services ==="
sudo systemctl restart pi-audio-engine
sudo systemctl restart apache2
sleep 2

echo ""
echo "=== Verification ==="
echo "Engine status:"
sudo systemctl is-active pi-audio-engine
echo ""
echo "Status files:"
ls -la /run/pi-audio/
echo ""
echo "Engine state:"
cat /run/pi-audio/engine-state.json 2>/dev/null || echo "(not yet written)"
echo ""
echo "Status adapter:"
/usr/local/bin/pi-audio-status 2>&1
