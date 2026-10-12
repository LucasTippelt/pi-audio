#!/bin/bash
# Add console error logging to app.js
set -e

APP_JS="/var/www/pi-audio/pi-audio-web/assets/app.js"

# Add console.error to the request function (after the throw error line)
sudo sed -i 's/    throw error;$/    console.error("[pi-audio] API error (" + action + "):", data.error || response.status, data);\n    throw error;/' "$APP_JS"

# Add console.log to refreshDspStatus for debugging
sudo sed -i 's/const data = await .request(.dsp-status.);/const data = await request("dsp-status");\n      console.log("[pi-audio] DSP status:", data);/' "$APP_JS"

# Add console.error to the catch block in refreshDspStatus
sudo sed -i 's/const message = ui.audioApplyError || `DSP status unavailable: ${error.message}`;/console.error("[pi-audio] DSP status error:", error);\n      const message = ui.audioApplyError || `DSP status unavailable: ${error.message}`;/' "$APP_JS"

echo "Frontend console logging added to $APP_JS"
