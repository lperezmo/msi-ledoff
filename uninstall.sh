#!/bin/bash
# Unload msi_ledoff (LEDs come back), stop loading it at boot, remove it from
# DKMS (all kernels), delete the /usr/src copy and the modprobe options file.
# Safe to run repeatedly.
set -euo pipefail

MOD=msi_ledoff
PKG=msi-ledoff

if [ "$(id -u)" -ne 0 ]; then
	echo "This script must be run as root: sudo $0" >&2
	exit 1
fi

if [ -d "/sys/module/$MOD" ]; then
	echo "Unloading $MOD (restores the original LED settings)"
	rmmod "$MOD"
fi

# First, so that boot-time loading is off even if a later step fails.  This
# file is the only thing that loads the module at boot.
rm -f "/etc/modules-load.d/$PKG.conf"

if command -v dkms >/dev/null; then
	dkms status "$PKG" | sed -n "s|^$PKG/\([^,: ]*\).*|\1|p" | sort -u |
	while read -r ver; do
		echo "Removing $PKG/$ver from DKMS"
		dkms remove "$PKG/$ver" --all </dev/null
	done
fi

for dir in /usr/src/"$PKG"-*; do
	[ -d "$dir" ] || continue
	echo "Deleting $dir"
	rm -rf "$dir"
done

OPTS="/etc/modprobe.d/$PKG-options.conf"
if [ -e "$OPTS" ]; then
	echo "Deleting $OPTS"
	rm -f "$OPTS"
fi

echo "$PKG removed."
