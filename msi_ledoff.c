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
 * Nothing else is written.  In particular CR 2C, E0, EB, ED (pin muxing read
 * by nct6775), the colour registers F0..FB, FD and FE are never written.
 *
 * Both tools agree on the E4 value, but msi-rgb also forces global CR 2C
 * bit 4 and E0 bits 7..5 on before it.  This module does not (they are shared
 * pin-mux bits); it only logs them so a "nothing happened" result can be
 * diagnosed from the kernel log.
 *
 * The module has no DMI modalias on purpose: it is only ever loaded by name
 * (modprobe, or /etc/modules-load.d), never automatically by udev.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/array_size.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/printk.h>
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
#define RGB_REG_ENABLE		0xe0	/* read-only here: logged, never written */
#define RGB_REG_CFG1		0xe4	/* bit 3 breathing, bits 2..0 blink/off */
#define RGB_REG_COLOUR		0xf0	/* f0..fb, read-only here */
#define RGB_COLOUR_LEN		12
#define RGB_REG_FD		0xfd	/* read-only here */
#define RGB_REG_FE		0xfe	/* read-only here */
#define RGB_REG_CFG3		0xff	/* bit 1 = RGB header enable */

#define CFG1_MODE_MASK		0x0f	/* the only E4 bits this module owns */
#define CFG1_MODE_OFF		0x01	/* blink field 001 = all lighting off */
#define CFG3_HEADER_EN		0x02	/* the only FF bit this module owns */

static bool dry_run;
module_param(dry_run, bool, 0444);
MODULE_PARM_DESC(dry_run,
		 "Check DMI and chip ID and log the LED registers, but change nothing (default: N)");

static bool header_off;
module_param(header_off, bool, 0444);
MODULE_PARM_DESC(header_off,
		 "Additionally clear the RGB header enable bit, CR FF bit 1 (default: N)");

static bool resume_reapply = true;
module_param(resume_reapply, bool, 0444);
MODULE_PARM_DESC(resume_reapply,
		 "Re-apply the LED-off setting after resume from suspend/hibernation (default: Y)");

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
static bool ledoff_cfg3_saved;	/* FF was changed by us; ledoff_cfg3_orig valid */
static u8 ledoff_cfg3_orig;

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
 * Write one LDN 0x12 register and verify it by reading it back.
 *
 * Firmware (SMI/AML) does not honour the muxed region and could select another
 * logical device behind our back, so the selection is checked right before
 * the write (nothing is written if it is wrong) and again after the read-back
 * (so a write that may have gone elsewhere is at least reported).
 */
static int ledoff_write_verify(int ioreg, int reg, u8 old, u8 val)
{
	u8 now, ld;

	ld = sio_inb(ioreg, SIO_REG_LDSEL);
	if (ld != SIO_LD_RGB) {
		pr_err("CR %02X: logical device changed to 0x%02x under us, nothing written\n",
		       reg, ld);
		return -EIO;
	}

	sio_outb(ioreg, reg, val);
	now = sio_inb(ioreg, reg);
	ld = sio_inb(ioreg, SIO_REG_LDSEL);
	if (ld != SIO_LD_RGB) {
		pr_err("CR %02X: logical device changed to 0x%02x during the write of 0x%02x (was 0x%02x); the write may have gone to that device\n",
		       reg, ld, val, old);
		return -EIO;
	}
	if (now != val) {
		pr_err("CR %02X: wrote 0x%02x (was 0x%02x) but read back 0x%02x\n",
		       reg, val, old, now);
		return -EIO;
	}
	pr_info("CR %02X: 0x%02x -> 0x%02x\n", reg, old, val);
	return 0;
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
	u8 colour[RGB_COLOUR_LEN];
	u8 old_ld, cr2c, e0, cfg1, fd, fe, cfg3, val;
	bool unchanged1 = false, unchanged3 = false;
	int i, err, ret = 0;
	u16 id;

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
		colour[i] = sio_inb(ioreg, RGB_REG_COLOUR + i);

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

		if (!header_off)
			break;
		val = cfg3 & ~CFG3_HEADER_EN;
		if (val != cfg3) {
			if (!ledoff_cfg3_saved) {
				ledoff_cfg3_orig = cfg3;
				ledoff_cfg3_saved = true;
			}
			ret = ledoff_write_verify(ioreg, RGB_REG_CFG3, cfg3, val);
		} else {
			unchanged3 = true;
		}
		break;

	case LEDOFF_OP_RESTORE:
		/* Only put back the bits this module owns; keep the rest. */
		if (ledoff_cfg1_saved) {
			val = (cfg1 & ~CFG1_MODE_MASK) |
			      (ledoff_cfg1_orig & CFG1_MODE_MASK);
			if (val != cfg1)
				ret = ledoff_write_verify(ioreg, RGB_REG_CFG1,
							  cfg1, val);
			ledoff_cfg1_saved = false;
		}
		if (ledoff_cfg3_saved) {
			val = (cfg3 & ~CFG3_HEADER_EN) |
			      (ledoff_cfg3_orig & CFG3_HEADER_EN);
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

	sio_outb(ioreg, SIO_REG_LDSEL, old_ld);
	sio_exit(ioreg);
	mutex_unlock(&ledoff_lock);

	/* Logged only now, to keep the time spent in config mode short. */
	pr_info("port 0x%02x: chip ID 0x%04x, LDN was 0x%02x, global CR2C=0x%02x; LDN 0x12 as found: E0=0x%02x E4=0x%02x FD=0x%02x FE=0x%02x FF=0x%02x F0..FB=%*ph\n",
		ioreg, id, old_ld, cr2c, e0, cfg1, fd, fe, cfg3,
		RGB_COLOUR_LEN, colour);
	if (op == LEDOFF_OP_DUMP)
		pr_info("dry run: lighting is currently %s (E4 bits 2..0 = %u), RGB header bit is %s, channel enables E0[7:5] = %u%u%u, CR2C bit 4 = %u; nothing changed\n",
			(cfg1 & 0x07) == CFG1_MODE_OFF ? "OFF" : "ON",
			cfg1 & 0x07,
			(cfg3 & CFG3_HEADER_EN) ? "on" : "off",
			!!(e0 & 0x80), !!(e0 & 0x40), !!(e0 & 0x20),
			!!(cr2c & 0x10));
	if (unchanged1)
		pr_info("CR E4 already says lighting off, not written\n");
	if (unchanged3)
		pr_info("CR FF RGB header bit already off, not written\n");
	return ret;

out_exit:
	sio_exit(ioreg);
	mutex_unlock(&ledoff_lock);
	return ret;
}

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

	pr_info("onboard lighting switched off; 'rmmod %s' restores it\n",
		DRVNAME);
	return 0;

err_restore:
	/* Undo a partial apply (no-op if nothing was changed). */
	if (ledoff_cfg1_saved || ledoff_cfg3_saved)
		ledoff_session(ledoff_port, LEDOFF_OP_RESTORE);
	return ret;
}

static void __exit ledoff_exit(void)
{
	if (dry_run)
		return;

	if (resume_reapply)
		unregister_pm_notifier(&ledoff_pm_nb);

	if (!ledoff_cfg1_saved && !ledoff_cfg3_saved) {
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
MODULE_VERSION("0.1.0");
MODULE_LICENSE("GPL");
