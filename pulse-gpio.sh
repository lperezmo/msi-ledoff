#!/bin/bash
# Blink one Super I/O GPIO output (4 times, 1 s each way) and leave it as it
# was, to see by eye what it drives (watch the LEDs). Needs msi_ledoff >= 0.5.0 loaded.
#
# This WRITES a hardware register. What the pin drives is unknown: the machine
# may reset or power off. Save your work first. If it dies mid-pulse and acts
# oddly afterwards, switch the power supply off at the back for 30 seconds.
set -euo pipefail

MOD=msi_ledoff
PARAM="/sys/module/$MOD/parameters/pulse"
PIN="${1:-}"

if [[ ! "$PIN" =~ ^[0-8][0-7]$ ]]; then
	echo "Usage: sudo $0 <group><bit>     e.g. sudo $0 73   (GPIO7 bit 3)" >&2
	exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "This script must be run as root: sudo $0 $PIN" >&2
	exit 1
fi
if [ ! -e "$PARAM" ]; then
	echo "$MOD is not loaded or is older than 0.5.0." >&2
	exit 1
fi

# In case the pin turns out to be reset or power.
sync
MARK="msi-ledoff-pulse: $PIN $$-$(date +%s)"
echo "$MARK" >/dev/kmsg
echo "Blinking GPIO$PIN 4 times over about 8 seconds, watch the LEDs..."
RC=0
echo "$PIN" >"$PARAM" || RC=$?
dmesg | sed -n "/$MARK/,\$p" | grep -F "$MOD: pulse" | sed 's/^\[[^]]*\] *//' || true
if [ "$RC" -eq 0 ]; then
	echo "Done, GPIO$PIN is back as it was."
fi
if [ "$RC" -ne 0 ]; then
	echo "Refused or failed (see the line above); pins that are inputs or in an inactive group are never written." >&2
	exit 1
fi
