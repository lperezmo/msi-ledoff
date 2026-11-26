#!/bin/bash
# Build + install msi_ledoff through DKMS, then do a DRY-RUN load only.
# Does NOT switch the LEDs off and does NOT enable anything at boot.
set -euo pipefail

MOD=msi_ledoff
PKG=msi-ledoff
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VER="$(sed -n 's/^PACKAGE_VERSION="\(.*\)"$/\1/p' "$HERE/dkms.conf")"
KVER="$(uname -r)"
SRC="/usr/src/$PKG-$VER"
BOOTCONF="/etc/modules-load.d/$PKG.conf"

if [ "$(id -u)" -ne 0 ]; then
	echo "This script must be run as root: sudo $0" >&2
	exit 1
fi
if [ -z "$VER" ]; then
	echo "Cannot read PACKAGE_VERSION from $HERE/dkms.conf" >&2
	exit 1
fi
for tool in dkms modinfo modprobe rmmod dmesg sed grep; do
	command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
if [ -d "/sys/module/$MOD" ]; then
	echo "$MOD is currently loaded. Run 'sudo rmmod $MOD' (the LEDs come back on) and re-run." >&2
	exit 1
fi

# The module has no modalias, so installing it does not make it load at boot.
# The only boot path is $BOOTCONF, written by enable-at-boot.sh.

echo "==> Removing any previously installed version of $PKG"
dkms status "$PKG" | sed -n "s|^$PKG/\([0-9][0-9A-Za-z.]*\)[,:].*|\1|p" | sort -u | while read -r OLD; do
	[ -n "$OLD" ] || continue
	echo "    $PKG/$OLD"
	dkms remove "$PKG/$OLD" --all </dev/null
	rm -rf "/usr/src/$PKG-$OLD"
done

echo "==> Copying sources to $SRC"
rm -rf "$SRC"
install -d -m 0755 "$SRC"
install -m 0644 "$HERE/msi_ledoff.c" "$HERE/Makefile" "$HERE/dkms.conf" "$SRC/"

echo "==> dkms add / build / install for kernel $KVER"
dkms add "$PKG/$VER"
dkms build "$PKG/$VER" -k "$KVER"
dkms install "$PKG/$VER" -k "$KVER"

# Other installed kernels that have headers (e.g. the GRUB fallback entry), so
# a boot-time load does not fail there.  Kernels installed later are handled
# by the DKMS kernel hooks.  Failures here are not fatal.
for build in /lib/modules/*/build; do
	[ -e "$build/Makefile" ] || continue
	OTHER="$(basename "$(dirname "$build")")"
	[ "$OTHER" != "$KVER" ] || continue
	echo "==> dkms install for kernel $OTHER"
	dkms install "$PKG/$VER" -k "$OTHER" ||
		echo "WARNING: could not build/install for kernel $OTHER; $MOD will be missing if you boot it." >&2
done

echo "==> Checking the module signature"
SIGNER="$(modinfo -k "$KVER" -F signer "$MOD" 2>/dev/null || true)"
if [ -z "$SIGNER" ]; then
	echo "The installed module is NOT signed; Secure Boot would reject it." >&2
	echo "Check /var/lib/dkms/mok.key, /var/lib/dkms/mok.pub and 'mokutil --list-enrolled'." >&2
	exit 1
fi
echo "Signed by: $SIGNER"
# Is that the DKMS key (the one enrolled in MOK)?  Informational only: a wrong
# key fails safe, the kernel simply refuses the dry-run load below.
MOKPUB=/var/lib/dkms/mok.pub
if command -v openssl >/dev/null && [ -r "$MOKPUB" ]; then
	SIGKEY="$(modinfo -k "$KVER" -F sig_key "$MOD" 2>/dev/null | tr -d ':\n' | tr 'a-f' 'A-F')"
	MOKSER="$(openssl x509 -inform DER -in "$MOKPUB" -noout -serial 2>/dev/null |
		sed 's/^serial=//' | tr -d '\n' | tr 'a-f' 'A-F')"
	if [ -n "$SIGKEY" ] && [ "$SIGKEY" = "$MOKSER" ]; then
		echo "Signing key matches $MOKPUB."
	else
		echo "WARNING: signing key ($SIGKEY) does not match $MOKPUB ($MOKSER)." >&2
		echo "         The kernel will reject the module unless that key is enrolled ('mokutil --list-enrolled')." >&2
	fi
fi

echo "==> Dry-run load (reads the chip ID and LED registers, changes nothing)"
MARK="$PKG-install: dry run $$-$(date +%s)"
echo "$MARK" >/dev/kmsg
RC=0
modprobe "$MOD" dry_run=1 || RC=$?
echo "----- kernel log -----"
dmesg | sed -n "/$MARK/,\$p" | grep -F "$MOD" || echo "(no $MOD lines in the kernel log)"
echo "----------------------"
if [ "$RC" -ne 0 ]; then
	echo "Dry-run load FAILED (modprobe exit code $RC). No hardware register was changed." >&2
	echo "If modprobe said 'Key was rejected by service', the signing key is not enrolled: check 'mokutil --list-enrolled'." >&2
	if [ -e "$BOOTCONF" ]; then
		rm -f "$BOOTCONF"
		echo "Boot-time loading had been enabled earlier; removed $BOOTCONF so a module that fails its dry run is not loaded at boot." >&2
	fi
	echo "The module stays installed but is not loaded at boot; run ./uninstall.sh to remove it." >&2
	exit 1
fi
rmmod "$MOD"

cat <<EOF

Dry run OK and the module is unloaded again. Nothing was changed.
Check the lines above: a chip ID of 0xd35x (NCT6795D) or 0xd45x (NCT6797D)
and plausible register values (not all 0xff).  Keep a copy of that log, in
particular E0, E4, FF and CR2C: it is what you need if the real load later
has no visible effect.

Next steps (manual, on purpose):
  sudo modprobe $MOD      # real load: LEDs should go off
  sudo rmmod $MOD         # undo: LEDs come back
  sudo ./enable-at-boot.sh    # once you are happy: load now and at every boot

Nothing loads this module at boot until you run enable-at-boot.sh.
EOF
