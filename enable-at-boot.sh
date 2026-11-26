#!/bin/bash
# Load msi_ledoff for real and, only if that works, load it at every boot.
set -euo pipefail

MOD=msi_ledoff
PKG=msi-ledoff
BOOTCONF="/etc/modules-load.d/$PKG.conf"

if [ "$(id -u)" -ne 0 ]; then
	echo "This script must be run as root: sudo $0" >&2
	exit 1
fi
if ! modinfo "$MOD" >/dev/null 2>&1; then
	echo "$MOD is not installed for kernel $(uname -r). Run ./install.sh first." >&2
	exit 1
fi

# A leftover dry-run instance does nothing; replace it with a real one.
if [ -d "/sys/module/$MOD" ] && [ "$(cat "/sys/module/$MOD/parameters/dry_run")" = "Y" ]; then
	echo "Unloading dry-run instance of $MOD"
	rmmod "$MOD"
fi

if [ -d "/sys/module/$MOD" ]; then
	echo "$MOD is already loaded (real mode)."
else
	MARK="$PKG-enable: load $$-$(date +%s)"
	echo "$MARK" >/dev/kmsg
	RC=0
	modprobe "$MOD" || RC=$?
	echo "----- kernel log -----"
	dmesg | sed -n "/$MARK/,\$p" | grep -F "$MOD" || echo "(no $MOD lines in the kernel log)"
	echo "----------------------"
	if [ "$RC" -ne 0 ] || [ ! -d "/sys/module/$MOD" ]; then
		echo "Loading $MOD FAILED; boot-time loading was NOT enabled." >&2
		exit 1
	fi
fi

if [ "$(cat "/sys/module/$MOD/parameters/dry_run")" != "N" ]; then
	echo "$MOD came up in dry-run mode (is there an 'options $MOD dry_run=1' line in /etc/modprobe.d?)." >&2
	echo "Boot-time loading was NOT enabled." >&2
	exit 1
fi

printf '%s\n' "# Written by $PKG enable-at-boot.sh" "$MOD" >"$BOOTCONF"

echo "$MOD is loaded and will be loaded at every boot ($BOOTCONF)."
echo "That file is the only thing that loads it at boot."

# The boot-time load uses only the options in /etc/modprobe.d, not whatever
# was typed on the modprobe command line for the instance loaded now.
MODCONF="$(modprobe -c 2>/dev/null || true)"
if [ "$(cat "/sys/module/$MOD/parameters/header_off")" = "Y" ] &&
   ! grep -Eq "^options[[:space:]]+$MOD[[:space:]].*header_off=(1|[Yy])" <<<"$MODCONF"; then
	echo "WARNING: $MOD is loaded with header_off=1, but no modprobe option makes that permanent." >&2
	echo "         The next boot will load it WITHOUT header_off. To keep it:" >&2
	echo "         echo 'options $MOD header_off=1' | sudo tee /etc/modprobe.d/$PKG-options.conf" >&2
fi

echo "Undo for this session: sudo rmmod $MOD"
echo "Stop loading at boot, keep it installed: sudo rm $BOOTCONF"
echo "Remove completely: sudo ./uninstall.sh"
