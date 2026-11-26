// SPDX-License-Identifier: GPL-2.0
/*
 * msi_ledoff - turn off the onboard LEDs of the MSI X470 GAMING PLUS (MS-7B79)
 *
 * The onboard (red) LEDs and the RGB header of this board are driven by the
 * Nuvoton NCT6795D/NCT6797D Super I/O chip, logical device 0x12.  With Secure
 * Boot / kernel lockdown, userspace cannot reach the Super I/O config ports,
 * so this tiny module does the one documented "all lighting off" write from
 * kernel space and undoes it on unload.
 *
 * Register knowledge comes from reverse engineering (the datasheet is not
 * public): nagisa/msi-rgb (src/main.rs) and ixjf/MSIRGB (Lighting.cpp).
 * Super I/O access conventions follow drivers/hwmon/nct6775-platform.c.
 *
 *   LDN 0x12, CR E4  bits 2..0 = 001 : all lighting off (board + header)
 *                    bit 3           : breathing/pulse (cleared for "off")
 *                    bits 7..4       : unknown, always preserved
 *   LDN 0x12, CR FF  bit 1           : RGB header enable (only touched when
 *                                      header_off=1), other bits preserved
 *
 *   LDN 0x12, CR E0  bits 7..5       : R/G/B channel enables.  msi-rgb notes
 *                                      that a channel whose bit is 0 "always
 *                                      receives the maximum brightness,
 *                                      regardless of setting".  This board's
 *                                      firmware leaves them 000, and with
 *                                      that the E4 "off" write alone was
 *                                      observed to have no visible effect
 *                                      (v0.1.0).  So they are set (unless
 *                                      channel_enable=0), as msi-rgb and
 *                                      MSIRGB do; bits 4..0 are preserved
 *                                      (nct6775 reads them for pin muxing).
 *
 *   LDN 0x12, CR FF  bits 4..2       : B/G/R invert (F = 0 %, 0 = 100 %).
 *   LDN 0x12, CR F0..FB              : colour frames, 4 each for R, G, B.
 *                                      This board's firmware leaves the
 *                                      invert bits set and the colours 00,
 *                                      i.e. every channel at 100 %.  With
 *                                      0.2.0 (E4 off + E0 enables) a light
 *                                      turned from red to white instead of
 *                                      going dark, so E4 = 001 does not blank
 *                                      this board.  Clearing the invert bits
 *                                      (invert=0, the default since 0.4.0)
 *                                      makes colour 00 mean 0 % and was
 *                                      confirmed on hardware to turn the
 *                                      lighting off.  colour= is an optional
 *                                      extra.  Both are restored on unload.
 *
 *   LDN 0x09, CR E1  bit 7 (GP27)    : the four red LEDs above the memory
 *                                      slots; cleared unless dimm_led_off=0,
 *                                      restored on unload.  See DIMM_LED_BIT.
 *
 * Nothing else is written unless a test pulse is asked for through the pulse
 * parameter (see ledoff_pulse_set).  In particular CR 2C, EB, ED (pin muxing
 * read by nct6775), FD and FE are never written.
 *
 * msi-rgb also forces global CR 2C bit 4 on ("without this pulsing does not
 * work").  This module does not; it only logs it.
 *
 * Read-only register dump (dump=1 at load, or write 1 to
 * /sys/module/msi_ledoff/parameters/dump): logs the GPIO configuration of the
 * chip so that two dumps (e.g. with different numbers of memory modules) can
 * be diffed.  Register indexes are from the public NCT6796D datasheet V0.6
 * (section 24); the NCT6795D has no public datasheet, so the names are a best
 * guess for this chip.  Nothing is written, and the GPIO status registers,
 * which clear on read, are left out.
 *
 * The module has no DMI modalias on purpose: it is only ever loaded by name
 * (modprobe, or /etc/modules-load.d), never automatically by udev.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/types.h>

#define DRVNAME			"msi_ledoff"

/* Super I/O global registers (same as nct6775-platform.c) */
#define SIO_REG_LDSEL		0x07	/* Logical device select */
#define SIO_REG_DEVID		0x20	/* Device ID (2 bytes) */
#define SIO_REG_CR2C		0x2c	/* global; read-only here: logged, never written */
#define SIO_NCT6795_ID		0xd350
#define SIO_NCT6797_ID		0xd450
#define SIO_ID_MASK		0xFFF8

/* Logical device 0x12: LED control (shared with fan/PWM pin muxing bits) */
#define SIO_LD_RGB		0x12
#define RGB_REG_ENABLE		0xe0	/* bits 7..5 = R/G/B channel enables */
#define RGB_REG_CFG1		0xe4	/* bit 3 breathing, bits 2..0 blink/off */
#define RGB_REG_COLOUR		0xf0	/* f0..fb, written only with colour= */
#define RGB_COLOUR_LEN		12
#define RGB_REG_FD		0xfd	/* read-only here */
#define RGB_REG_FE		0xfe	/* read-only here */
#define RGB_REG_CFG3		0xff	/* bit 1 = RGB header enable */

/*
 * The row of four red LEDs above the memory slots (lit per populated slot)
 * is switched by GP27: logical device 9, CR E1 bit 7, high = lit.  Found by
 * pulsing the GPIO outputs one at a time on a rev 2.0 board.  The pin is GP27
 * only while global CR 2C bits 4..3 are 00 (otherwise it is MLED).
 */
#define SIO_LD_GPIO2		0x09
#define GPIO2_EN_BIT		0x01	/* CR 30 bit 0: GPIO2 group active */
#define GPIO2_REG_DIR		0xe0	/* 1 = input */
#define GPIO2_REG_DATA		0xe1
#define DIMM_LED_BIT		0x80	/* GP27 */
#define CR2C_PIN98_MASK		0x18

#define CFG1_MODE_MASK		0x0f	/* the only E4 bits this module owns */
#define CFG1_MODE_OFF		0x01	/* blink field 001 = all lighting off */
#define CFG3_HEADER_EN		0x02	/* FF bit owned with header_off=1 */
#define CFG3_INVERT_MASK		0x1c	/* FF bits owned with invert=0/1 */
#define ENABLE_CHAN_MASK		0xe0	/* the only E0 bits this module owns */

static bool dry_run;
module_param(dry_run, bool, 0444);
MODULE_PARM_DESC(dry_run,
		 "Check DMI and chip ID and log the LED registers, but change nothing (default: N)");

static bool header_off;
module_param(header_off, bool, 0444);
MODULE_PARM_DESC(header_off,
		 "Additionally clear the RGB header enable bit, CR FF bit 1 (default: N)");

static bool channel_enable = true;
module_param(channel_enable, bool, 0444);
MODULE_PARM_DESC(channel_enable,
		 "Also set the R/G/B channel enable bits, CR E0 bits 7..5, which the off setting needs to take effect on this board (default: Y)");

static int invert;
module_param(invert, int, 0444);
MODULE_PARM_DESC(invert,
		 "CR FF bits 4..2, B/G/R invert: 0 clear so colour 00 means dark (default), 1 set, -1 leave alone");

static int colour = -1;
module_param(colour, int, 0444);
MODULE_PARM_DESC(colour,
		 "Byte to write to all twelve colour registers F0..FB: -1 leave alone (default), 0..255");

static bool dimm_led_off = true;
module_param(dimm_led_off, bool, 0444);
MODULE_PARM_DESC(dimm_led_off,
		 "Also switch off the four LEDs above the memory slots, GP27 (default: Y)");

static bool resume_reapply = true;
module_param(resume_reapply, bool, 0444);
MODULE_PARM_DESC(resume_reapply,
		 "Re-apply the LED-off setting after resume from suspend/hibernation (default: Y)");

/*
 * Read-only dump, see the header comment.  Left out on purpose because the
 * datasheet marks them Read-Clear: LDN 7 E3/E7/F7, LDN 8 E3/F3, LDN 9
 * E3/E7/E8/F7 (GPIO status).  The watchdog registers are left out too.
 */
#define DUMP_MAX_REGS		20
#define DUMP_GLOBAL		0xff	/* not a logical device: global CRs */

struct ledoff_dump_block {
	u8 ldn;
	const char *what;
	u8 n;
	u8 regs[DUMP_MAX_REGS];
};

static const struct ledoff_dump_block ledoff_dump_tbl[] = {
	{ DUMP_GLOBAL, "pin muxing", 15,
	  { 0x1a, 0x1b, 0x1c, 0x1d, 0x22, 0x24, 0x25, 0x26, 0x27, 0x28,
	    0x2a, 0x2b, 0x2c, 0x2d, 0x2f } },
	/* 30 enable; GPIO7 E0 dir/E1 data/E2 inv, GPIO8 E4/E5/E6, GPIO6 F4/F5/F6 */
	{ 0x07, "GPIO6/7/8", 13,
	  { 0x30, 0xe0, 0xe1, 0xe2, 0xe4, 0xe5, 0xe6, 0xec, 0xed,
	    0xf4, 0xf5, 0xf6, 0xf8 } },
	/* 30 enable; GPIO0 E0/E1/E2, GPIO1 F0/F1/F2 */
	{ 0x08, "GPIO0/1", 11,
	  { 0x30, 0x60, 0x61, 0xe0, 0xe1, 0xe2, 0xe4, 0xf0, 0xf1, 0xf2,
	    0xf4 } },
	/* 30 enable; GPIO2 E0/E1/E2, GPIO3 E4/E5/E6, GPIO4 F0/F1/F2, GPIO5 F4/F5/F6 */
	{ 0x09, "GPIO2/3/4/5", 17,
	  { 0x30, 0xe0, 0xe1, 0xe2, 0xe4, 0xe5, 0xe6, 0xe9, 0xea, 0xeb,
	    0xee, 0xf0, 0xf1, 0xf2, 0xf4, 0xf5, 0xf6 } },
	{ 0x0b, "front panel LEDs", 4, { 0xe6, 0xf6, 0xf7, 0xf8 } },
	/* E0..E7 = GPIO1..GPIO8, E9 = GPIO0; bit 0 push-pull, 1 open drain */
	{ 0x0f, "push-pull/open-drain", 9,
	  { 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe9 } },
	{ 0x12, "LED control", 20,
	  { 0xe0, 0xe4, 0xe5, 0xe6, 0xeb, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4,
	    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfd, 0xfe, 0xff } },
	{ 0x15, "fading LED", 6, { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5 } },
};

enum ledoff_op {
	LEDOFF_OP_DUMP,		/* read and log only */
	LEDOFF_OP_APPLY,	/* turn lighting off, remember what was changed */
	LEDOFF_OP_RESTORE,	/* put the remembered bits back */
};

/* Serialises sessions and protects the saved state below. */
static DEFINE_MUTEX(ledoff_lock);
static int ledoff_port;		/* config port the chip was found at */
static bool ledoff_cfg1_saved;	/* E4 was changed by us; ledoff_cfg1_orig valid */
static u8 ledoff_cfg1_orig;
static bool ledoff_e0_saved;	/* E0 was changed by us; ledoff_e0_orig valid */
static u8 ledoff_e0_orig;
static bool ledoff_cfg3_saved;	/* FF was changed by us; ledoff_cfg3_orig valid */
static u8 ledoff_cfg3_orig;
static bool ledoff_dimm_saved;	/* GP27 was changed by us; ledoff_dimm_orig valid */
static u8 ledoff_dimm_orig;
static bool ledoff_colour_saved; /* F0..FB changed by us; _orig valid */
static u8 ledoff_colour_orig[RGB_COLOUR_LEN];

static const struct dmi_system_id ledoff_dmi_table[] = {
	{
		.ident = "MSI X470 GAMING PLUS (MS-7B79)",
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR,
					"Micro-Star International Co., Ltd."),
			DMI_EXACT_MATCH(DMI_BOARD_NAME,
					"X470 GAMING PLUS (MS-7B79)"),
		},
	},
	{ }
};
/*
 * Deliberately no MODULE_DEVICE_TABLE(dmi, ...): that would make udev load
 * this hardware-writing module on every boot as soon as it is installed.
 */

/*
 * MSIRGB, where the E4 handling comes from, supports the MS-7B79 only in
 * board revisions 1.x to 4.x.  Refuse anything else.
 */
static bool ledoff_board_rev_ok(void)
{
	const char *ver = dmi_get_system_info(DMI_BOARD_VERSION);

	return ver && ver[0] >= '1' && ver[0] <= '4' && ver[1] == '.';
}

/*
 * Super I/O access, same protocol and same muxed region as nct6775-platform.c
 * so that the two cannot interleave if nct6775 is ever loaded.
 */
static inline void sio_outb(int ioreg, int reg, int val)
{
	outb(reg, ioreg);
	outb(val, ioreg + 1);
}

static inline int sio_inb(int ioreg, int reg)
{
	outb(reg, ioreg);
	return inb(ioreg + 1);
}

static int sio_enter(int ioreg)
{
	/* May sleep until another muxed user (e.g. nct6775) releases it. */
	if (!request_muxed_region(ioreg, 2, DRVNAME))
		return -EBUSY;

	outb(0x87, ioreg);
	outb(0x87, ioreg);

	return 0;
}

static void sio_exit(int ioreg)
{
	outb(0xaa, ioreg);
	outb(0x02, ioreg);
	outb(0x02, ioreg + 1);
	release_region(ioreg, 2);
}

static bool ledoff_id_ok(u16 id)
{
	switch (id & SIO_ID_MASK) {
	case SIO_NCT6795_ID:
	case SIO_NCT6797_ID:
		return true;
	default:
		return false;
	}
}

/*
 * Write one register of logical device @ldn and verify it by reading it back.
 *
 * Firmware (SMI/AML) does not honour the muxed region and could select another
 * logical device behind our back, so the selection is checked right before
 * the write (nothing is written if it is wrong) and again after the read-back
 * (so a write that may have gone elsewhere is at least reported).
 */
static int ledoff_write_verify_ld(int ioreg, u8 ldn, int reg, u8 old, u8 val)
{
	u8 now, ld;

	ld = sio_inb(ioreg, SIO_REG_LDSEL);
	if (ld != ldn) {
		pr_err("CR %02X: logical device changed to 0x%02x under us, nothing written\n",
		       reg, ld);
		return -EIO;
	}

	sio_outb(ioreg, reg, val);
	now = sio_inb(ioreg, reg);
	ld = sio_inb(ioreg, SIO_REG_LDSEL);
	if (ld != ldn) {
		pr_err("CR %02X: logical device changed to 0x%02x during the write of 0x%02x (was 0x%02x); the write may have gone to that device\n",
		       reg, ld, val, old);
		return -EIO;
	}
	if (now != val) {
		pr_err("CR %02X: wrote 0x%02x (was 0x%02x) but read back 0x%02x\n",
		       reg, val, old, now);
		return -EIO;
	}
	pr_info("LDN %02X CR %02X: 0x%02x -> 0x%02x\n", ldn, reg, old, val);
	return 0;
}

static int ledoff_write_verify(int ioreg, int reg, u8 old, u8 val)
{
	return ledoff_write_verify_ld(ioreg, SIO_LD_RGB, reg, old, val);
}

/*
 * One complete, self-contained Super I/O session: take the region, enter
 * config mode, verify the chip ID, do @op, restore the logical device
 * selection, leave config mode, release the region, then log what was found.
 *
 * The chip ID is re-verified in every session and nothing other than the
 * config-mode key is written before it has passed.
 *
 * Returns -ENODEV if no supported chip answers at @ioreg.
 */
static int ledoff_session(int ioreg, enum ledoff_op op)
{
	u8 colour_now[RGB_COLOUR_LEN];
	u8 old_ld, cr2c, e0, cfg1, fd, fe, cfg3, val, mask3;
	u8 g2en = 0, g2dir = 0, g2data = 0;
	bool unchanged0 = false, unchanged1 = false, unchanged3 = false;
	bool dimm_read = false, dimm_skip = false, dimm_already = false;
	int i, err, ret = 0;
	u16 id;

	mask3 = (header_off ? CFG3_HEADER_EN : 0) |
		(invert >= 0 ? CFG3_INVERT_MASK : 0);

	mutex_lock(&ledoff_lock);

	err = sio_enter(ioreg);
	if (err) {
		pr_err("I/O ports 0x%02x-0x%02x are busy\n", ioreg, ioreg + 1);
		mutex_unlock(&ledoff_lock);
		return err;
	}

	id = (sio_inb(ioreg, SIO_REG_DEVID) << 8) |
	     sio_inb(ioreg, SIO_REG_DEVID + 1);
	if (!ledoff_id_ok(id)) {
		pr_info("port 0x%02x: chip ID 0x%04x is not NCT6795D/NCT6797D, leaving it alone\n",
			ioreg, id);
		ret = -ENODEV;
		goto out_exit;
	}

	/*
	 * nct6775 does not save/restore the logical device selection, but
	 * putting it back is harmless and leaves the chip exactly as found.
	 */
	cr2c = sio_inb(ioreg, SIO_REG_CR2C);
	old_ld = sio_inb(ioreg, SIO_REG_LDSEL);
	sio_outb(ioreg, SIO_REG_LDSEL, SIO_LD_RGB);

	e0 = sio_inb(ioreg, RGB_REG_ENABLE);
	cfg1 = sio_inb(ioreg, RGB_REG_CFG1);
	fd = sio_inb(ioreg, RGB_REG_FD);
	fe = sio_inb(ioreg, RGB_REG_FE);
	cfg3 = sio_inb(ioreg, RGB_REG_CFG3);
	for (i = 0; i < RGB_COLOUR_LEN; i++)
		colour_now[i] = sio_inb(ioreg, RGB_REG_COLOUR + i);

	switch (op) {
	case LEDOFF_OP_DUMP:
		break;

	case LEDOFF_OP_APPLY:
		val = (cfg1 & ~CFG1_MODE_MASK) | CFG1_MODE_OFF;
		if (val != cfg1) {
			if (!ledoff_cfg1_saved) {
				ledoff_cfg1_orig = cfg1;
				ledoff_cfg1_saved = true;
			}
			ret = ledoff_write_verify(ioreg, RGB_REG_CFG1, cfg1, val);
			if (ret)
				break;
		} else {
			unchanged1 = true;
		}

		/* E4 first, so the engine is already "off" when it takes over. */
		if (channel_enable) {
			val = e0 | ENABLE_CHAN_MASK;
			if (val != e0) {
				if (!ledoff_e0_saved) {
					ledoff_e0_orig = e0;
					ledoff_e0_saved = true;
				}
				ret = ledoff_write_verify(ioreg, RGB_REG_ENABLE,
							  e0, val);
				if (ret)
					break;
			} else {
				unchanged0 = true;
			}
		}

		if (colour >= 0) {
			for (i = 0; i < RGB_COLOUR_LEN && !ret; i++) {
				if (colour_now[i] == (u8)colour)
					continue;
				if (!ledoff_colour_saved) {
					memcpy(ledoff_colour_orig, colour_now,
					       RGB_COLOUR_LEN);
					ledoff_colour_saved = true;
				}
				ret = ledoff_write_verify(ioreg,
							  RGB_REG_COLOUR + i,
							  colour_now[i],
							  (u8)colour);
			}
			if (ret)
				break;
		}

		val = cfg3;
		if (header_off)
			val &= ~CFG3_HEADER_EN;
		if (invert == 0)
			val &= ~CFG3_INVERT_MASK;
		else if (invert > 0)
			val |= CFG3_INVERT_MASK;
		if (val != cfg3) {
			if (!ledoff_cfg3_saved) {
				ledoff_cfg3_orig = cfg3;
				ledoff_cfg3_saved = true;
			}
			ret = ledoff_write_verify(ioreg, RGB_REG_CFG3, cfg3, val);
		} else if (mask3) {
			unchanged3 = true;
		}
		break;

	case LEDOFF_OP_RESTORE:
		/* Only put back the bits this module owns; keep the rest. */
		if (ledoff_e0_saved) {
			val = (e0 & ~ENABLE_CHAN_MASK) |
			      (ledoff_e0_orig & ENABLE_CHAN_MASK);
			if (val != e0)
				ret = ledoff_write_verify(ioreg, RGB_REG_ENABLE,
							  e0, val);
			ledoff_e0_saved = false;
		}
		if (ledoff_cfg1_saved) {
			val = (cfg1 & ~CFG1_MODE_MASK) |
			      (ledoff_cfg1_orig & CFG1_MODE_MASK);
			if (val != cfg1) {
				err = ledoff_write_verify(ioreg, RGB_REG_CFG1,
							  cfg1, val);
				if (!ret)
					ret = err;
			}
			ledoff_cfg1_saved = false;
		}
		if (ledoff_colour_saved) {
			for (i = 0; i < RGB_COLOUR_LEN; i++) {
				if (colour_now[i] == ledoff_colour_orig[i])
					continue;
				err = ledoff_write_verify(ioreg,
							  RGB_REG_COLOUR + i,
							  colour_now[i],
							  ledoff_colour_orig[i]);
				if (!ret)
					ret = err;
			}
			ledoff_colour_saved = false;
		}
		if (ledoff_cfg3_saved) {
			val = (cfg3 & ~mask3) | (ledoff_cfg3_orig & mask3);
			if (val != cfg3) {
				err = ledoff_write_verify(ioreg, RGB_REG_CFG3,
							  cfg3, val);
				if (!ret)
					ret = err;
			}
			ledoff_cfg3_saved = false;
		}
		break;
	}

	/* The memory slot LEDs live in another logical device. */
	if (dimm_led_off || ledoff_dimm_saved) {
		sio_outb(ioreg, SIO_REG_LDSEL, SIO_LD_GPIO2);
		g2en = sio_inb(ioreg, 0x30);
		g2dir = sio_inb(ioreg, GPIO2_REG_DIR);
		g2data = sio_inb(ioreg, GPIO2_REG_DATA);
		dimm_read = true;

		if (op == LEDOFF_OP_APPLY && dimm_led_off && !ret) {
			/* Only if the pin is GP27, active and an output. */
			if (!(g2en & GPIO2_EN_BIT) || (g2dir & DIMM_LED_BIT) ||
			    (cr2c & CR2C_PIN98_MASK)) {
				dimm_skip = true;
			} else if (g2data & DIMM_LED_BIT) {
				if (!ledoff_dimm_saved) {
					ledoff_dimm_orig = g2data;
					ledoff_dimm_saved = true;
				}
				ret = ledoff_write_verify_ld(ioreg, SIO_LD_GPIO2,
							     GPIO2_REG_DATA, g2data,
							     g2data & ~DIMM_LED_BIT);
			} else {
				dimm_already = true;
			}
		} else if (op == LEDOFF_OP_RESTORE && ledoff_dimm_saved) {
			val = (g2data & ~DIMM_LED_BIT) |
			      (ledoff_dimm_orig & DIMM_LED_BIT);
			if (val != g2data && !(g2dir & DIMM_LED_BIT)) {
				err = ledoff_write_verify_ld(ioreg, SIO_LD_GPIO2,
							     GPIO2_REG_DATA,
							     g2data, val);
				if (!ret)
					ret = err;
			}
			ledoff_dimm_saved = false;
		}
	}

	sio_outb(ioreg, SIO_REG_LDSEL, old_ld);
	sio_exit(ioreg);
	mutex_unlock(&ledoff_lock);

	/* Logged only now, to keep the time spent in config mode short. */
	pr_info("port 0x%02x: chip ID 0x%04x, LDN was 0x%02x, global CR2C=0x%02x; LDN 0x12 as found: E0=0x%02x E4=0x%02x FD=0x%02x FE=0x%02x FF=0x%02x F0..FB=%*ph\n",
		ioreg, id, old_ld, cr2c, e0, cfg1, fd, fe, cfg3,
		RGB_COLOUR_LEN, colour_now);
	if (op == LEDOFF_OP_DUMP)
		pr_info("dry run: lighting is currently %s (E4 bits 2..0 = %u), RGB header bit is %s, channel enables E0[7:5] = %u%u%u, CR2C bit 4 = %u; nothing changed\n",
			(cfg1 & 0x07) == CFG1_MODE_OFF ? "OFF" : "ON",
			cfg1 & 0x07,
			(cfg3 & CFG3_HEADER_EN) ? "on" : "off",
			!!(e0 & 0x80), !!(e0 & 0x40), !!(e0 & 0x20),
			!!(cr2c & 0x10));
	if (dimm_read)
		pr_info("memory slot LEDs (GP27) as found: LDN 09 CR30=0x%02x E0=0x%02x E1=0x%02x, %s\n",
			g2en, g2dir, g2data,
			(g2data & DIMM_LED_BIT) ? "lit" : "off");
	if (dimm_skip)
		pr_warn("GP27 is not an active GPIO output here (CR2C=0x%02x), memory slot LEDs left alone\n",
			cr2c);
	if (dimm_already)
		pr_info("memory slot LEDs already off, not written\n");
	if (unchanged0)
		pr_info("CR E0 channel enables already set, not written\n");
	if (unchanged1)
		pr_info("CR E4 already says lighting off, not written\n");
	if (unchanged3)
		pr_info("CR FF already as requested, not written\n");
	return ret;

out_exit:
	sio_exit(ioreg);
	mutex_unlock(&ledoff_lock);
	return ret;
}

/*
 * Read-only session: log the registers listed in ledoff_dump_tbl.  The only
 * writes are the config-mode key and the logical device selection (CR 07),
 * which is put back afterwards.
 */
static int ledoff_gpio_dump(int ioreg)
{
	static u8 vals[ARRAY_SIZE(ledoff_dump_tbl)][DUMP_MAX_REGS];
	bool moved[ARRAY_SIZE(ledoff_dump_tbl)] = { };
	const struct ledoff_dump_block *b;
	char line[DUMP_MAX_REGS * 6 + 1];
	int i, j, len, err;
	u8 old_ld;
	u16 id;

	mutex_lock(&ledoff_lock);	/* also protects vals[] */

	err = sio_enter(ioreg);
	if (err) {
		pr_err("I/O ports 0x%02x-0x%02x are busy\n", ioreg, ioreg + 1);
		mutex_unlock(&ledoff_lock);
		return err;
	}

	id = (sio_inb(ioreg, SIO_REG_DEVID) << 8) |
	     sio_inb(ioreg, SIO_REG_DEVID + 1);
	if (!ledoff_id_ok(id)) {
		sio_exit(ioreg);
		mutex_unlock(&ledoff_lock);
		pr_err("dump: chip ID 0x%04x is not NCT6795D/NCT6797D, nothing read\n",
		       id);
		return -ENODEV;
	}

	old_ld = sio_inb(ioreg, SIO_REG_LDSEL);
	for (i = 0; i < ARRAY_SIZE(ledoff_dump_tbl); i++) {
		b = &ledoff_dump_tbl[i];
		if (b->ldn != DUMP_GLOBAL)
			sio_outb(ioreg, SIO_REG_LDSEL, b->ldn);
		for (j = 0; j < b->n; j++)
			vals[i][j] = sio_inb(ioreg, b->regs[j]);
		/* Firmware may have selected another device meanwhile. */
		if (b->ldn != DUMP_GLOBAL &&
		    sio_inb(ioreg, SIO_REG_LDSEL) != b->ldn)
			moved[i] = true;
	}
	sio_outb(ioreg, SIO_REG_LDSEL, old_ld);
	sio_exit(ioreg);

	pr_info("dump: chip ID 0x%04x at port 0x%02x, read only, nothing changed\n",
		id, ioreg);
	for (i = 0; i < ARRAY_SIZE(ledoff_dump_tbl); i++) {
		b = &ledoff_dump_tbl[i];
		len = 0;
		for (j = 0; j < b->n; j++)
			len += scnprintf(line + len, sizeof(line) - len,
					 " %02X=%02x", b->regs[j], vals[i][j]);
		if (b->ldn == DUMP_GLOBAL)
			pr_info("dump: global (%s):%s\n", b->what, line);
		else
			pr_info("dump: LDN %02X (%s):%s%s\n", b->ldn, b->what,
				line,
				moved[i] ? " [logical device changed under us, values unreliable]" : "");
	}

	mutex_unlock(&ledoff_lock);
	return 0;
}

static bool ledoff_dump_at_load;

/* dump=1 on the modprobe command line, or written to the sysfs parameter. */
static int ledoff_dump_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret;

	ret = kstrtobool(val, &on);
	if (ret)
		return ret;
	if (!on)
		return 0;
	if (!ledoff_port) {
		/* Not initialised yet: ledoff_init() does it. */
		ledoff_dump_at_load = true;
		return 0;
	}
	return ledoff_gpio_dump(ledoff_port);
}

static const struct kernel_param_ops ledoff_dump_ops = {
	.set = ledoff_dump_set,
};
module_param_cb(dump, &ledoff_dump_ops, NULL, 0200);
MODULE_PARM_DESC(dump,
		 "Write 1 to log a read-only dump of the GPIO and LED registers (changes nothing)");

/*
 * Test pulse: blink one GPIO output (flipped for pulse_ms, back for pulse_ms,
 * pulse_count times), to see by eye what the pin drives.  Written as two digits to the sysfs parameter,
 * group then bit: "73" is GPIO7 bit 3.
 *
 * Refused unless the group is active and the pin is already an output, so no
 * pin is ever turned from an input into an output (which could fight whatever
 * else drives that line).  Only the data register of that group is written;
 * per the datasheet, writes to bits of input pins are ignored.
 *
 * What a pin drives on this board is unknown: it may reset or power off the
 * machine.  These registers sit on standby power, so if the machine dies
 * during the pulse the flipped bit can survive a reboot; switching the power
 * supply off for a while clears it.
 */
struct ledoff_gpio_group {
	u8 ldn;
	u8 en_bit;	/* in CR 30 of that logical device */
	u8 dir;		/* direction register; data is dir + 1 */
};

static const struct ledoff_gpio_group ledoff_gpio_groups[] = {
	[0] = { 0x08, 1, 0xe0 },
	[1] = { 0x08, 7, 0xf0 },
	[2] = { 0x09, 0, 0xe0 },
	[3] = { 0x09, 1, 0xe4 },
	[4] = { 0x09, 2, 0xf0 },
	[5] = { 0x09, 3, 0xf4 },
	[6] = { 0x07, 0, 0xf4 },
	[7] = { 0x07, 1, 0xe0 },
	[8] = { 0x07, 2, 0xe4 },
};

static unsigned int pulse_ms = 1000;
module_param(pulse_ms, uint, 0644);
MODULE_PARM_DESC(pulse_ms,
		 "How long each flip of a test pulse lasts, and the gap between flips, 100..10000 ms (default: 1000)");

static unsigned int pulse_count = 4;
module_param(pulse_count, uint, 0644);
MODULE_PARM_DESC(pulse_count,
		 "How many times a test pulse flips the pin, 1..20 (default: 4)");

/*
 * One half of a pulse: flip the bit (@restore false) or put it back to
 * @orig (@restore true).  Called with ledoff_lock held.
 */
static int ledoff_pulse_half(int ioreg, unsigned int grp, unsigned int bit,
			     bool restore, u8 *orig)
{
	const struct ledoff_gpio_group *g = &ledoff_gpio_groups[grp];
	u8 old_ld, en, dir, data = 0, val = 0, now = 0, ld;
	const char *why = NULL;
	int err;
	u16 id;

	err = sio_enter(ioreg);
	if (err)
		return err;

	id = (sio_inb(ioreg, SIO_REG_DEVID) << 8) |
	     sio_inb(ioreg, SIO_REG_DEVID + 1);
	if (!ledoff_id_ok(id)) {
		sio_exit(ioreg);
		return -ENODEV;
	}

	old_ld = sio_inb(ioreg, SIO_REG_LDSEL);
	sio_outb(ioreg, SIO_REG_LDSEL, g->ldn);
	en = sio_inb(ioreg, 0x30);
	dir = sio_inb(ioreg, g->dir);
	data = sio_inb(ioreg, g->dir + 1);

	if (!(en & BIT(g->en_bit))) {
		why = "the group is not active";
	} else if (dir & BIT(bit)) {
		why = "the pin is an input";
	} else if (sio_inb(ioreg, SIO_REG_LDSEL) != g->ldn) {
		why = "the logical device changed under us";
	} else {
		if (restore)
			val = (data & ~BIT(bit)) | (*orig & BIT(bit));
		else
			val = data ^ BIT(bit);
		sio_outb(ioreg, g->dir + 1, val);
		now = sio_inb(ioreg, g->dir + 1);
		ld = sio_inb(ioreg, SIO_REG_LDSEL);
		if (ld != g->ldn)
			why = "the logical device changed during the write";
	}

	sio_outb(ioreg, SIO_REG_LDSEL, old_ld);
	sio_exit(ioreg);

	if (why) {
		pr_err("pulse GPIO%u%u: %s; %s\n", grp, bit, why,
		       restore ? "the pin may still be flipped" : "nothing written");
		return -EIO;
	}
	if (!restore)
		*orig = data;
	pr_info("pulse GPIO%u%u: LDN %02X CR %02X 0x%02x -> 0x%02x (reads back 0x%02x)%s\n",
		grp, bit, g->ldn, g->dir + 1, data, val, now,
		restore ? ", restored" : "");
	return 0;
}

static int ledoff_pulse_set(const char *val, const struct kernel_param *kp)
{
	unsigned int grp, bit, ms, n, i;
	u8 orig = 0;
	int ret = 0;

	if (!val || val[0] < '0' || val[0] > '8' || val[1] < '0' ||
	    val[1] > '7' || (val[2] && val[2] != '\n'))
		return -EINVAL;
	grp = val[0] - '0';
	bit = val[1] - '0';
	if (!ledoff_port)
		return -EAGAIN;	/* only after the module has loaded */

	ms = clamp(pulse_ms, 100U, 10000U);
	n = clamp(pulse_count, 1U, 20U);

	/* Blink: flipped for ms, back for ms, n times; ends as it started. */
	mutex_lock(&ledoff_lock);
	for (i = 0; i < n && !ret; i++) {
		if (i)
			msleep(ms);
		ret = ledoff_pulse_half(ledoff_port, grp, bit, false, &orig);
		if (ret)
			break;
		msleep(ms);
		ret = ledoff_pulse_half(ledoff_port, grp, bit, true, &orig);
	}
	mutex_unlock(&ledoff_lock);
	return ret;
}

static const struct kernel_param_ops ledoff_pulse_ops = {
	.set = ledoff_pulse_set,
};
module_param_cb(pulse, &ledoff_pulse_ops, NULL, 0200);
MODULE_PARM_DESC(pulse,
		 "Write two digits, group and bit (73 = GPIO7 bit 3), to blink that GPIO output pulse_count times and leave it as it was. Only pins that are already outputs. May reset the machine.");

/*
 * Firmware may reprogram the LED registers on resume.  PM_POST_* runs in
 * process context after devices (including nct6775, if loaded) have resumed,
 * so taking the muxed region here is fine.
 */
static int ledoff_pm_notify(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	switch (action) {
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		if (ledoff_session(ledoff_port, LEDOFF_OP_APPLY))
			pr_err("re-applying after resume failed\n");
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block ledoff_pm_nb = {
	.notifier_call = ledoff_pm_notify,
};

static int __init ledoff_init(void)
{
	/* Preferred port first: MSIRGB hard-codes 0x4e on these boards. */
	static const int ports[] = { 0x4e, 0x2e };
	enum ledoff_op op = dry_run ? LEDOFF_OP_DUMP : LEDOFF_OP_APPLY;
	int i, ret = -ENODEV;

	if (colour > 255 || colour < -1 || invert > 1 || invert < -1) {
		pr_err("invalid colour= or invert= value\n");
		return -EINVAL;
	}
	if (!dmi_check_system(ledoff_dmi_table)) {
		pr_err("unsupported board (need MSI X470 GAMING PLUS (MS-7B79)), doing nothing\n");
		return -ENODEV;
	}
	if (!ledoff_board_rev_ok()) {
		pr_err("unsupported board revision (need 1.x to 4.x), doing nothing\n");
		return -ENODEV;
	}

	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		ret = ledoff_session(ports[i], op);
		if (ret != -ENODEV) {
			ledoff_port = ports[i];
			break;
		}
	}

	if (ret == -ENODEV) {
		pr_err("no NCT6795D/NCT6797D Super I/O found, doing nothing\n");
		return ret;
	}
	if (ret)
		goto err_restore;

	if (ledoff_dump_at_load && ledoff_gpio_dump(ledoff_port))
		pr_err("register dump failed\n");

	if (dry_run) {
		pr_info("dry run finished; unload with 'rmmod %s' before loading for real\n",
			DRVNAME);
		return 0;
	}

	if (resume_reapply) {
		ret = register_pm_notifier(&ledoff_pm_nb);
		if (ret) {
			pr_err("cannot register PM notifier (%d)\n", ret);
			goto err_restore;
		}
	}

	pr_info("lighting-off setting applied; 'rmmod %s' restores the original values\n",
		DRVNAME);
	return 0;

err_restore:
	/* Undo a partial apply (no-op if nothing was changed). */
	if (ledoff_e0_saved || ledoff_cfg1_saved || ledoff_cfg3_saved ||
	    ledoff_colour_saved || ledoff_dimm_saved)
		ledoff_session(ledoff_port, LEDOFF_OP_RESTORE);
	return ret;
}

static void __exit ledoff_exit(void)
{
	if (dry_run)
		return;

	if (resume_reapply)
		unregister_pm_notifier(&ledoff_pm_nb);

	if (!ledoff_e0_saved && !ledoff_cfg1_saved && !ledoff_cfg3_saved &&
	    !ledoff_colour_saved && !ledoff_dimm_saved) {
		pr_info("nothing was changed, nothing to restore\n");
		return;
	}

	if (ledoff_session(ledoff_port, LEDOFF_OP_RESTORE))
		pr_err("restoring the LED registers failed; a reboot resets them\n");
	else
		pr_info("original LED settings restored\n");
}

module_init(ledoff_init);
module_exit(ledoff_exit);

MODULE_AUTHOR("msi_ledoff contributors");
MODULE_DESCRIPTION("Turn off the onboard LEDs of the MSI X470 GAMING PLUS (MS-7B79) via the NCT679x Super I/O");
MODULE_VERSION("0.6.0");
MODULE_LICENSE("GPL");
