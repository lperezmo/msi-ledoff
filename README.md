# msi-ledoff

Kernel module that switches off the onboard (red) LEDs, the RGB header and
the row of red LEDs above the memory slots of the
**MSI X470 GAMING PLUS (MS-7B79)**. The LEDs hang off the Nuvoton Super I/O
chip, which userspace (OpenRGB) cannot reach with Secure Boot / lockdown on.
The module is signed by DKMS with the enrolled MOK key, so Secure Boot stays on.

Status: 0.6.0 is confirmed on one board (rev 2.0, BIOS A.MA, NCT6795D
`0xd352`): with the module loaded the board is completely dark. Other
revisions are untested. The lighting register meanings come from reverse
engineering (nagisa/msi-rgb, ixjf/MSIRGB); the GPIO register layout comes from
the public NCT6796D datasheet, since the NCT6795D has none.

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

   Since 0.2.0 (unless `channel_enable=0`): after `E4`, also writes CR
   `E0 = E0 | 0xE0` (the R/G/B channel enables; low five bits preserved), with
   the same checks. On this board the firmware leaves those bits `000`, and
   with them clear the `E4` write alone had no visible effect (tested with
   0.1.0). `rmmod` restores them.

   Since 0.4.0 (unless `invert=-1`): also clears CR `FF` bits 4..2 (B/G/R
   invert). The firmware leaves them set with all colour registers `00`,
   which means 100 % on every channel; cleared, `00` means dark. **This is the
   write that actually turns the lighting off on the tested board** (rev 2.0,
   NCT6795D `0xd352`): `E4` alone did nothing, `E4`+`E0` turned a light from
   red to white, adding the invert clear turned it off. `rmmod` restores it.

   Since 0.6.0 (unless `dimm_led_off=0`): also clears LDN `09` CR `E1` bit 7,
   the GPIO output GP27. On the tested board that pin switches the row of
   four red LEDs above the memory slots (one per populated slot), which do
   not react to any LDN `0x12` register. It was found by blinking the chip's
   GPIO outputs one at a time (see "Test pulse" below). The bit is written
   only if the GPIO2 group is active, the pin is an output and global CR `2C`
   bits 4..3 are `00` (the pin is GP27, not MLED); otherwise a warning is
   logged and it is left alone. `rmmod` restores it.

Nothing else is written by default: not CR `2C`, `EB`, `ED`, `FD` or `FE`
(a test pulse, below, writes one GPIO data bit, only when asked).
The colour registers are written only with the optional `colour=` parameter.

On unload (`rmmod`) the same session is run and every bit the module changed
is put back to its saved value: the low four bits of `E4`, the channel
enables in `E0`, the `FF` bits, GP27 and, if `colour=` was used, the colour
registers.

Nothing is put back on reboot or power-off (modules are not unloaded then).
The firmware is expected to reprogram the chip at POST, but that is untested:
if the off state survives a warm reboot into another OS, a full power-off, or
`rmmod` before rebooting, brings the LEDs back.

After resume from suspend/hibernation the "off" write is applied again (a PM
notifier; disable with `resume_reapply=0`).

Parameters:

| Parameter | Default | Effect |
|---|---|---|
| `dry_run=1` | off | ID check and log the registers, change nothing |
| `header_off=1` | off | also clear the RGB header enable bit |
| `dimm_led_off=0` | on | leave the memory slot LEDs (GP27) alone |
| `channel_enable=0` | on | do not set the R/G/B channel enables in `E0` |
| `invert=-1` / `1` | `0` | leave the invert bits alone / set them |
| `colour=0..255` | `-1` | write that byte to all twelve colour registers |
| `resume_reapply=0` | on | do not re-apply after resume |
| `dump=1` | off | one read-only register dump at load (see "Register dump") |

`pulse`, `pulse_ms` and `pulse_count` are described under "Test pulse". There
is no way to bypass the DMI or chip ID check.

The module has no modalias: nothing loads it automatically. It is loaded only
by name, by `modprobe` or by the one file `enable-at-boot.sh` writes. After a
`dry_run=1` load it stays loaded doing nothing; `rmmod` it before a real load.

## Install and test

```sh
sudo ./install.sh
```

Copies the sources to `/usr/src/msi-ledoff-<version>` (removing any older
installed version first), runs `dkms add/build/install` for the running kernel and then for every other installed kernel that has
headers, checks the module is signed (and whether the key is the DKMS MOK
key), loads it once with `dry_run=1`, prints the kernel log and unloads it.
It does not enable anything at boot. If the dry run fails and boot loading had
been enabled earlier, it turns boot loading off again.

Look at the log: the chip ID should be `0xd35x` or `0xd45x` and the register
values should not all be `0xff`. Keep that log (`E0`, `E4`, `FF`, `CR2C` and
the GP27 line): if the real load has no visible effect on another board
revision, those values are what is needed to work out why.

To upgrade from an older version, run `sudo rmmod msi_ledoff` first (the
installer refuses while the module is loaded), then `sudo ./install.sh` and
`sudo modprobe msi_ledoff`. Boot loading stays enabled.

Then try it for real:

```sh
sudo modprobe msi_ledoff      # all LEDs should go off
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
not write), and the GPIO outputs other than GP27 have unknown functions (the
module writes them only for a test pulse). If anything looks wrong after
loading:

1. `sudo rmmod msi_ledoff` puts the changed bits back.
2. If that does not help, reboot; the firmware reprograms the chip at POST.
   If the module is enabled at boot, first run `sudo ./uninstall.sh` (or see
   the section above).
3. Last resort, per the msi-rgb authors: clear CMOS.

## Register dump (read only)

Since 0.5.0 the module can log the chip's GPIO and LED configuration without
changing anything. It was added to find what drives the LEDs above the
memory slots (answer: GP27, see above) and is kept for similar questions.

```sh
sudo ./dump-gpio.sh 4sticks     # writes dumps/4sticks.txt
```

or by hand: `echo 1 | sudo tee /sys/module/msi_ledoff/parameters/dump`, then
`sudo dmesg | grep 'msi_ledoff: dump'`. `dump=1` on the `modprobe` command
line does one dump at load. It works whether the module was loaded for real
or with `dry_run=1`.

Dumps taken in two states can be compared with `diff`. Pins whose direction
bit is `0` are outputs, the candidates for a test pulse.

On the tested board the outputs are GP11/GP16/GP17 (the blue/green/red
lighting channels: they become PWM outputs while LDN `12` CR `E0` bits 5/6/7
are set), GP27 (memory slot LEDs) and GP24, GP25, GP30, GP31, GP32, GP47,
GP50, GP71 (function unknown; GP24 is probably the memory VPP power-good
signal and should be left alone).

What is read: global CR `1A`-`1D`, `22`, `24`-`28`, `2A`-`2D`, `2F` (pin
muxing); LDN `07`, `08`, `09` (GPIO0 to GPIO8: enable, direction, data,
inversion, multi-function select); LDN `0B` `E6`/`F6`-`F8` (front panel LEDs);
LDN `0F` (push-pull or open drain per pin); LDN `12` and LDN `15` (LED
control). Per GPIO group the registers come in the order direction (1 =
input), data, inversion.

The register indexes are from the public NCT6796D datasheet (V0.6, section
24). The NCT6795D on this board has no public datasheet, so the names are a
best guess for it. The GPIO status registers (LDN `07` `E3`/`E7`/`F7`, LDN
`08` `E3`/`F3`, LDN `09` `E3`/`E7`/`E8`/`F7`) clear when read and are left
out, as are the watchdog registers. The only writes in a dump session are the
config-mode key and the logical device selection (CR `07`), which is put back.

## Test pulse (writes a register)

To find out by eye what a GPIO output drives:

```sh
sudo ./pulse-gpio.sh 73      # GPIO7 bit 3: blinks 4 times, 1 s each way
```

It is refused unless that GPIO group is active and the pin is already an
output (direction bit `0` in the dump), so no input is ever turned into an
output. Only the one data bit changes, and it ends as it started. `pulse_ms`
(100 to 10000, default 1000) and `pulse_count` (1 to 20, default 4) in
`/sys/module/msi_ledoff/parameters/` set the length and number of blinks.

Apart from GP27 and the three lighting channels, what these pins drive on
this board is not known. A pulse may reset or power
off the machine (the script runs `sync` first), or briefly change something
else such as a fan mode. The registers sit on standby power, so if the machine
dies during a pulse the flipped bit can survive a reboot: switch the power
supply off at the back for 30 seconds to clear it.

## Build by hand (no root needed)

```sh
make            # or: make KVER=<kernel version>
make clean
```
