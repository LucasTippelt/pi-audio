#!/bin/bash
# Fix permissions and add console logging
set -e

# 1. Fix config file permissions so engine (lucas) can read them
echo "Fixing config file permissions..."
sudo chmod 664 /etc/pi-audio/config/current.json
sudo find /etc/pi-audio/presets -name "*.json" -exec sudo chmod 664 {} \;

# 2. Fix api.php to write with 664 instead of 660 (so engine can read new files)
echo "Fixing api.php chmod..."
sudo sed -i 's/@chmod($path, 0660);/@chmod($path, 0664);/' /var/www/pi-audio/pi-audio-web/api.php

# 3. Add console.error to the request() function in app.js
echo "Adding console logging to app.js..."
sudo sed -i '/    throw error;$/i\    console.error("[pi-audio] API error (" + action + "):", data.error || response.status, data);' /var/www/pi-audio/pi-audio-web/assets/app.js

# 4. Add console.log for DSP status response
sudo sed -i '/const data = await request(.dsp-status.);$/a\      console.log("[pi-audio] DSP status response:", data);' /var/www/pi-audio/pi-audio-web/assets/app.js

echo ""
echo "Done. Run: sudo systemctl restart pi-audio-engine && sudo systemctl restart apache2"
