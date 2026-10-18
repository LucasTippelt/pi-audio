#!/bin/bash
# Deploy the Pi Audio DSP rewrite engine to /usr/local/bin/ and restart service.
# The systemd service must be stopped before replacing the running binary.
# Run from: /var/www/pi-audio/pi-audio-dsp-rewrite/
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BINARY="$BUILD_DIR/pi-audio-engine"
DEST="/usr/local/bin/pi-audio-engine"
SERVICE="pi-audio-engine.service"
UNIT_FILE="/etc/systemd/system/$SERVICE"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "============================================"
echo "  Pi Audio DSP Rewrite — Deploy"
echo "============================================"
echo ""

# --- Pre-flight checks ---
if [ ! -f "$BINARY" ]; then
    echo -e "${RED}Binary not found at $BINARY${NC}"
    echo "Run ./build.sh first."
    exit 1
fi

# --- Step 1: Configure deploy ---
echo -e "${YELLOW}[1/5] Configure engine:${NC}"
echo ""

# Ask for mode (sim or alsa)
while true; do
    read -p "  Mode [sim/alsa] (default: alsa): " MODE_INPUT
    case "$MODE_INPUT" in
        sim|SIM) MODE="sim"; break ;;
        alsa|ALSA|"") MODE="alsa"; break ;;
        *) echo "  Invalid. Enter 'sim' or 'alsa'." ;;
    esac
done

# Ask for sample rate
while true; do
    read -p "  Sample rate (default: 48000): " RATE_INPUT
    if [ -z "$RATE_INPUT" ]; then
        RATE=48000
        break
    fi
    if [[ "$RATE_INPUT" =~ ^[0-9]+$ ]]; then
        RATE=$RATE_INPUT
        break
    fi
    echo "  Invalid. Enter a number."
done

# Ask for buffer size
while true; do
    read -p "  Buffer size (default: 128): " BUF_INPUT
    if [ -z "$BUF_INPUT" ]; then
        BUF=128
        break
    fi
    if [[ "$BUF_INPUT" =~ ^[0-9]+$ ]]; then
        BUF=$BUF_INPUT
        break
    fi
    echo "  Invalid. Enter a number."
done

# --- Interactive device selector helpers (alsa mode only) ---
if [ "$MODE" = "alsa" ]; then

list_alsa_devices() {
    local kind="$1"  # "capture" or "playback"
    local cmd
    if [ "$kind" = "capture" ]; then
        cmd="arecord -L"
    else
        cmd="aplay -L"
    fi

    local raw
    raw=$($cmd 2>/dev/null) || true
    if [ -z "$raw" ]; then
        echo ""
        return
    fi

    # Parse arecord/aplay -L output: device name on bare line, description on indented lines.
    # Output format: device_name  CardName — type_description
    echo "$raw" | awk -v kind="$kind" '
    function label(d) {
        if (d ~ /^hw:/)          return "direct hardware access"
        if (d ~ /^plughw:/)     return "hardware with software conversions"
        if (d == "default")      return "system default"
        if (d ~ /^default:/)    return "card-specific default"
        if (d ~ /^sysdefault:/) return "card-specific sysdefault"
        if (d ~ /^front:/)      return "front channel"
        if (d == "null")         return "discard / generate zero"
        if (d ~ /^dsnoop:/)     return "shared capture"
        if (d ~ /^dmix:/)       return "shared playback"
        if (d ~ /^iec958:/)     return "S/PDIF digital output"
        if (d ~ /^surround/)    return d
        return d
    }
    /^[^ \t]/ {
        if (dev != "" && desc != "") print dev "  " desc " — " label(dev)
        dev = $0; desc = ""; next
    }
    /^    / { gsub(/^    /, ""); if (desc == "") desc = $0; next }
    END {
        if (dev != "" && desc != "") print dev "  " desc " — " label(dev)
    }'
}

select_device() {
    local prompt="$1"
    local kind="$2"   # "capture" or "playback"
    local var_name="$3"

    local devices=()
    local raw_lines
    raw_lines=$(list_alsa_devices "$kind")

    if [ -z "$raw_lines" ]; then
        echo "  No $kind devices detected. Using 'default'."
        eval "$var_name=\"default\""
        return
    fi

    # Build array: index -> "hw:X,Y", display -> human-readable
    local displays=()
    while IFS= read -r line; do
        devices+=("${line%%  *}")   # device ID before double-space separator
        displays+=("${line#*  }")   # display text after double-space separator
    done <<< "$raw_lines"

    # Prepend "default" as first option
    devices=("default" "${devices[@]}")
    displays=("default (system default)" "${displays[@]}")

    local total=${#devices[@]}
    local current=0
    local height=7
    local window_start=0

    while true; do
        clear 2>/dev/null || printf '\033[2J\033[H'
        echo ""
        echo -e "  ${GREEN}Select $prompt device:${NC}"
        echo "  (Arrow keys to navigate, Enter to confirm)"
        echo ""

        # Compute visible window
        local window_end=$((window_start + height))
        if [ $((total - window_start)) -le $height ]; then
            window_start=$((total - height))
            [ $window_start -lt 0 ] && window_start=0
            window_end=$((window_start + height))
        fi

        local i=$window_start
        while [ $i -lt $total ] && [ $((i - window_start)) -lt $height ]; do
            if [ $i -eq $current ]; then
                echo -e "  ${GREEN}> ${displays[$i]}${NC}"
            else
                echo "    ${displays[$i]}"
            fi
            i=$((i + 1))
        done

        echo ""
        echo -e "  ${YELLOW}Current selection:${NC} ${displays[$current]}"
        echo ""

        # Read a single keypress (arrow up/down, enter)
        IFS= read -r -s -n3 key
        case "$key" in
            $'\x1b[A')  # Up arrow
                if [ $current -gt 0 ]; then
                    current=$((current - 1))
                    if [ $current -lt $window_start ]; then
                        window_start=$current
                    fi
                fi
                ;;
            $'\x1b[B')  # Down arrow
                if [ $current -lt $((total - 1)) ]; then
                    current=$((current + 1))
                    if [ $current -ge $((window_start + height)) ]; then
                        window_start=$((current - height + 1))
                    fi
                fi
                ;;
            "")          # Enter (empty 3-byte read = just Enter)
                eval "$var_name=\"${devices[$current]}\""
                return
                ;;
        esac
    done
}

select_device "capture" "capture" "CAPDEV"
echo ""
select_device "playback" "playback" "PLAYDEV"
fi

# Ask for duration (sim mode only)
if [ "$MODE" = "sim" ]; then
    while true; do
        read -p "  Sim duration seconds (default: 5): " DUR_INPUT
        if [ -z "$DUR_INPUT" ]; then
            DUR=5
            break
        fi
        # Accept integer or float
        if [[ "$DUR_INPUT" =~ ^[0-9]+\.?[0-9]*$ ]]; then
            DUR=$DUR_INPUT
            break
        fi
        echo "  Invalid. Enter a number."
    done
fi

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Configuration:${NC}"
echo -e "${GREEN}  Mode: $MODE | Rate: $RATE | Buffer: $BUF${NC}"
if [ "$MODE" = "alsa" ]; then
    echo -e "${GREEN}  Capture: $CAPDEV | Playback: $PLAYDEV${NC}"
fi
if [ "$MODE" = "sim" ]; then
    echo -e "${GREEN}  Duration: ${DUR}s${NC}"
fi
echo -e "${GREEN}============================================${NC}"
echo ""

# --- Step 2: Stop service ---
echo -e "${YELLOW}[2/5] Stopping $SERVICE ...${NC}"
sudo systemctl stop "$SERVICE" || true
sleep 1
echo -e "${GREEN}      Stopped.${NC}"

# --- Step 3: Install binary ---
echo -e "${YELLOW}[3/5] Installing to $DEST ...${NC}"
sudo cp "$BINARY" "$DEST"
sudo chmod 755 "$DEST"
echo -e "${GREEN}      Installed.${NC}"

# --- Step 4: Update systemd ExecStart with chosen args ---
echo -e "${YELLOW}[4/5] Updating service configuration...${NC}"

ARGS="--rate $RATE --buffer $BUF"
if [ "$MODE" = "alsa" ]; then
    ARGS="$ARGS --capture-dev $CAPDEV --playback-dev $PLAYDEV"
fi
if [ "$MODE" = "sim" ]; then
    ARGS="$ARGS --sim"
    export PI_AUDIO_SIM_HAL=1
fi

NEW_EXEC="ExecStart=$DEST /etc/pi-audio/config/current.json /var/log/pi-audio/engine.log $ARGS"

# Update the unit file: replace existing ExecStart line, or append if missing.
if grep -q '^ExecStart=' "$UNIT_FILE" 2>/dev/null; then
    sudo sed -i "s|^ExecStart=.*|$NEW_EXEC|" "$UNIT_FILE"
else
    # Add ExecStart before [Install] section
    sudo sed -i '/^\[Install\]/i '"$NEW_EXEC"'' "$UNIT_FILE"
fi

sudo systemctl daemon-reload
echo -e "${GREEN}      Updated: $NEW_EXEC${NC}"

# --- Step 5: Start service ---
echo -e "${YELLOW}[5/5] Starting $SERVICE ...${NC}"
sudo systemctl start "$SERVICE"
sleep 2

if systemctl is-active --quiet "$SERVICE"; then
    echo -e "${GREEN}      Running.${NC}"
else
    echo -e "${RED}      Service failed to start.${NC}"
    echo "Check: journalctl -eu $SERVICE --no-pager -n 30"
    exit 1
fi

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Deploy complete — engine is running.${NC}"
echo -e "${GREEN}============================================${NC}"
