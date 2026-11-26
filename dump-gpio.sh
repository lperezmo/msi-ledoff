#!/bin/bash
# Save a read-only dump of the Super I/O GPIO and LED registers to
# dumps/<label>.txt, so dumps taken in different states can be diffed.
# Changes no hardware register. Needs msi_ledoff >= 0.5.0 loaded.
set -euo pipefail

MOD=msi_ledoff
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARAM="/sys/module/$MOD/parameters/dump"
LABEL="${1:-}"

if [ -z "$LABEL" ] || [[ ! "$LABEL" =~ ^[A-Za-z0-9._-]+$ ]]; then
	echo "Usage: sudo $0 <label>     e.g. sudo $0 4sticks" >&2
	echo "The label may contain letters, digits, '.', '_' and '-'." >&2
	exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "This script must be run as root: sudo $0 $LABEL" >&2
	exit 1
fi
if [ ! -d "/sys/module/$MOD" ]; then
	echo "$MOD is not loaded. Run 'sudo modprobe $MOD' (or 'sudo modprobe $MOD dry_run=1') first." >&2
	exit 1
fi
if [ ! -e "$PARAM" ]; then
	echo "The loaded $MOD has no dump parameter (older than 0.5.0)." >&2
	echo "Run 'sudo rmmod $MOD', 'sudo ./install.sh', 'sudo modprobe $MOD' and try again." >&2
	exit 1
fi

OUT="$HERE/dumps/$LABEL.txt"
if [ -e "$OUT" ]; then
	echo "$OUT already exists; pick another label or delete it." >&2
	exit 1
fi

MARK="msi-ledoff-dump: $LABEL $$-$(date +%s)"
echo "$MARK" >/dev/kmsg
echo 1 >"$PARAM"

install -d -m 0755 "$HERE/dumps"
dmesg | sed -n "/$MARK/,\$p" | grep -F "$MOD: dump:" |
	sed -e 's/^\[[^]]*\] *//' -e "s/^$MOD: dump: //" >"$OUT" || true
if [ ! -s "$OUT" ]; then
	rm -f "$OUT"
	echo "No dump lines found in the kernel log; see 'sudo dmesg | tail'." >&2
	exit 1
fi
if [ -n "${SUDO_UID:-}" ]; then
	chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$HERE/dumps"
fi

cat "$OUT"
echo
echo "Saved to $OUT"
OTHERS="$(find "$HERE/dumps" -name '*.txt' ! -name "$LABEL.txt" -printf '%f\n' | sort)"
if [ -n "$OTHERS" ]; then
	echo "Compare with an earlier dump, e.g.:"
	echo "$OTHERS" | while read -r f; do
		echo "  diff $HERE/dumps/$f $OUT"
	done
fi
