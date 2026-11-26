# msi-ledoff

Kernel module that switches off the onboard (red) LEDs and RGB header of the
**MSI X470 GAMING PLUS (MS-7B79)**. The LEDs hang off the Nuvoton Super I/O
chip, which userspace (OpenRGB) cannot reach with Secure Boot / lockdown on.
The module is signed by DKMS with the enrolled MOK key, so Secure Boot stays on.

Status: builds cleanly, **not yet tested on hardware**. Register meanings come
from reverse engineering (nagisa/msi-rgb, ixjf/MSIRGB); there is no public
datasheet.

## What it does

On load:

1. Refuses unless DMI says vendor `Micro-Star International Co., Ltd.` and
   board `X470 GAMING PLUS (MS-7B79)` (exact match), board version `1.x` to
   `4.x` (the revisions MSIRGB supports).
2. For config port `0x4e`, then `0x2e`: takes the port pair with
   `request_muxed_region` (same as `nct6775`), enters config mode
   (`0x87 0x87`), reads the chip ID (CR `20`/`21`). Continues only if
   `(id & 0xfff8)` is `0xd350` (NCT6795D) or `0xd450` (NCT6797D); otherwise
   logs the ID, leaves config mode and gives up with `-ENODEV`.
3. Reads global CR `2C`, remembers the selected logical device (CR `07`),
   selects LDN `0x12`, reads `E0 E4 FD FE FF F0..FB`.
4. Writes CR `E4 = (E4 & 0xf0) | 0x01` (blink field `001` = all lighting off,
   breathing bit cleared, upper four bits kept) and reads it back. Skipped if
   `E4` already has that value. CR `07` is checked to still be `0x12` right
   before the write (if not: nothing is written, error) and after it.
5. Only with `header_off=1`: writes CR `FF = FF & ~0x02` (RGB header enable),
   with the same checks.
6. Puts CR `07` back, leaves config mode (`0xaa`, then the `nct6775` idle
   sequence), releases the ports, and only then logs what it read.

Nothing else is ever written: not CR `2C`, `E0`, `EB`, `ED`, the colour
registers, `FD` or `FE`.

On unload (`rmmod`) the same session is run and the saved low four bits of
`E4` (and bit 1 of `FF`, if it was changed) are put back.

Nothing is put back on reboot or power-off (modules are not unloaded then).
The firmware is expected to reprogram the chip at POST, but that is untested:
if the off state survives a warm reboot into another OS, a full power-off, or
`rmmod` before rebooting, brings the LEDs back.

After resume from suspend/hibernation the "off" write is applied again (a PM
notifier; disable with `resume_reapply=0`).

Parameters: `dry_run=1` (ID check + log the registers, change nothing),
`header_off=1`, `resume_reapply=0`. There is no way to bypass the DMI or chip
ID check.

The module has no modalias: nothing loads it automatically. It is loaded only
by name, by `modprobe` or by the one file `enable-at-boot.sh` writes. After a
`dry_run=1` load it stays loaded doing nothing; `rmmod` it before a real load.

## Install and test

```sh
sudo ./install.sh
```

Copies the sources to `/usr/src/msi-ledoff-0.1.0`, runs `dkms add/build/install`
for the running kernel and then for every other installed kernel that has
headers, checks the module is signed (and whether the key is the DKMS MOK
key), loads it once with `dry_run=1`, prints the kernel log and unloads it.
It does not enable anything at boot. If the dry run fails and boot loading had
been enabled earlier, it turns boot loading off again.

Look at the log: the chip ID should be `0xd35x` or `0xd45x` and the register
values should not all be `0xff`. **Save that log**, in particular `E0`, `E4`,
`FF` and `CR2C`. The other tools switch the LED function on (`CR2C` bit 4,
`E0` bits 7..5) before writing `E4`; this module does not touch those shared
bits, so if the firmware left them in another state the real load may have no
visible effect, and those values are what is needed to work out why.

Then try it for real:

```sh
sudo modprobe msi_ledoff      # LEDs should go off
sudo dmesg | grep msi_ledoff
sudo rmmod msi_ledoff         # LEDs come back
```

If the onboard LEDs go off but a strip on the RGB header stays lit, try
`sudo modprobe msi_ledoff header_off=1`; to make that permanent put
`options msi_ledoff header_off=1` in `/etc/modprobe.d/msi-ledoff-options.conf`
(the boot-time load only sees options from that directory, not ones typed on
the `modprobe` command line; `enable-at-boot.sh` warns if it is missing).

## Enable at boot

```sh
sudo ./enable-at-boot.sh
```

Loads the module for real and, only if that worked, writes
`/etc/modules-load.d/msi-ledoff.conf`. That file is the only thing that loads
the module at boot. DKMS rebuilds and re-signs it for new kernels
automatically.

The LEDs stay on during POST and early boot until the module loads.

## Undo

- This session only: `sudo rmmod msi_ledoff`
- Stop loading at boot but keep it installed:
  `sudo rm /etc/modules-load.d/msi-ledoff.conf`
- Everything: `sudo ./uninstall.sh` (rmmod, removes the boot config first,
  then the DKMS module for all kernels, `/usr/src/msi-ledoff-*` and
  `/etc/modprobe.d/msi-ledoff-options.conf`)

## If the machine misbehaves or will not boot

Skip the module for one boot (works with Secure Boot on):

1. At the GRUB menu, highlight the normal entry and press `e`.
2. Go to the line that starts with `linux` and add
   ` module_blacklist=msi_ledoff` (underscore, with a space before it) at the
   end of that line.
3. Press `Ctrl-X` or `F10` to boot. The change is not saved.
4. Once booted, run `sudo ./uninstall.sh` (or just remove
   `/etc/modules-load.d/msi-ledoff.conf`).

The GRUB "recovery mode" entry alone does not help: it still loads the modules
listed in `/etc/modules-load.d`.

## If fans or sensors act oddly

LDN `0x12` also holds fan/PWM pin configuration bits (which this module does
not write). If anything looks wrong after loading:

1. `sudo rmmod msi_ledoff` puts the changed bits back.
2. If that does not help, reboot; the firmware reprograms the chip at POST.
   If the module is enabled at boot, first run `sudo ./uninstall.sh` (or see
   the section above).
3. Last resort, per the msi-rgb authors: clear CMOS.

## Build by hand (no root needed)

```sh
make            # or: make KVER=<kernel version>
make clean
```
