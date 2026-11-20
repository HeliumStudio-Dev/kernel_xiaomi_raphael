// SPDX-License-Identifier: GPL-2.0
/*
 * Dummy /dev/iorap_dev node for Xiaomi's "launch boost" preread interface.
 *
 * Why this exists
 * ---------------
 * The vendor ROM ships /system_ext/bin/iorapd and starts it from
 * init.launch_boost.rc.  The first thing iorapd does in main() is
 *
 *	open("/dev/iorap_dev", O_RDWR)
 *
 * and the stock vendor kernel backs that node with an out-of-tree driver
 * (vendor/xiaomi/proprietary/file_system/io_prefetcher/launch_boost).  This
 * tree has no such driver, so the open returns -ENOENT, main() gives up, and
 * init restarts the service after its default five second delay, forever:
 *
 *	E iorapd  : Cannot open /dev/iorap_dev, errno=2
 *	E iorapd  : Failed to open device
 *
 * iorapd does *not* need the driver to work, though.  Every ioctl it issues is
 * optional by design:
 *
 *   - LB_GET_VERSION failing makes it log "treat as v2" and fall back to the
 *     v2 preread implementation;
 *   - LB_GET_PREREAD_INFO failing makes it log "preread failed" and skip the
 *     preread for that launch.
 *
 * So a node that always opens and reports a preread engine that supports
 * nothing keeps iorapd alive without ever handing it data it could act on.
 * Putting a plain file at /dev/iorap_dev by hand does the same thing, which is
 * how this was confirmed before writing the driver.
 *
 * Two details still have to be right:
 *
 *   - Node mode.  The ROM ships no ueventd rule for this path, so nothing
 *     relaxes the 0600 devtmpfs default and iorapd, which runs as "system",
 *     would get EACCES instead of ENOENT.  misc_devnode() honours
 *     miscdevice.mode on this tree, so ask for 0666.  (mi_metis gets away with
 *     the default only because nothing unprivileged opens it.)
 *
 *   - SELinux label.  /dev/iorap_dev is already labelled lb_device by
 *     system_ext_file_contexts and the artd domain -- iorapd's exec label is
 *     artd_exec -- is already allowed ioctl/read/write/open on it, so ueventd's
 *     coldboot restorecon is all that is needed.  Nothing to do here.
 *
 * Expected result in logcat:
 *
 *	I iorapd  : Driver version: 2.0.0
 *	I iorapd  : Using LbReadaheadV2 for driver v2
 *	I iorapd  : LbReadaheadV2: Ready!
 *	I iorapd  : launch boost iorapd Service started!
 *
 * TODO
 * ----
 * This is a stub, not the real thing: no file ranges are ever reported, so
 * launch boost prereading stays off and iorapd only survives.  The proper fix
 * is porting the vendor driver, or disabling the service with
 * `setprop persist.sys.stability.PrereadEnable false`.
 */

#undef pr_fmt
#define pr_fmt(fmt) "iorap_dev: " fmt

#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

/*
 * ioctl numbers used by iorapd, recovered from the shipped binary:
 *
 *	LB_GET_VERSION		_IOR('L', 20, u32)			0x80044c14
 *	LB_GET_PREREAD_INFO	_IOWR('L', 7, struct lb_preread_info)	0xc1084c07
 *	LB_GET_PREREAD_ENTRY	LB_GET_PREREAD_INFO + n   (n = 1, 2, ...)	0xc1084c08 ...
 *
 * iorapd issues the header ioctl first and, only when it succeeds, one entry
 * ioctl per preread range the kernel claimed to have.  We never report a
 * range, so the entry ioctls stay unused; both are refused the same way.
 */
#define LB_IOC_TYPE		0x4c	/* 'L' */
#define LB_NR_GET_PREREAD_INFO	7
#define LB_NR_GET_VERSION	20

/*
 * Version reported through LB_GET_VERSION, encoded the way iorapd expects:
 * (major << 16) | (minor << 8) | patch.  2.0.0 is exactly what iorapd falls
 * back to when the ioctl fails, so reporting it explicitly only removes the
 * "LB_GET_VERSION ioctl failed, treat as v2" warning from the boot log.
 * Change this constant if a future ROM expects another version.
 */
#define IORAP_VERSION		((2 << 16) | (0 << 8) | 0)

static int iorap_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int iorap_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long iorap_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) != LB_IOC_TYPE)
		return -ENOTTY;

	if (_IOC_NR(cmd) == LB_NR_GET_VERSION) {
		u32 __user *uver = (u32 __user *)arg;

		if (put_user(IORAP_VERSION, uver))
			return -EFAULT;

		return 0;
	}

	/*
	 * nr 7 asks for the preread ranges of a file, nr 8+ for the individual
	 * entries.  The real driver fills in the ranges it wants read ahead;
	 * this one has none to offer, and iorapd copes with the failure on its
	 * own, so refuse instead of handing back a buffer we did not fill.
	 */
	return -ENOTTY;
}

static const struct file_operations iorap_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.open		= iorap_open,
	.release	= iorap_release,
	.unlocked_ioctl	= iorap_ioctl,
	.compat_ioctl	= iorap_ioctl,
};

static struct miscdevice iorap_miscdev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "iorap_dev",
	.fops		= &iorap_fops,
	.mode		= 0666,
};

static int __init xiaomi_iorapd_init(void)
{
	int ret;

	ret = misc_register(&iorap_miscdev);
	if (ret) {
		pr_err("failed to register /dev/%s: %d\n",
		       iorap_miscdev.name, ret);
		return ret;
	}

	pr_info("registered /dev/%s (misc minor %d), reporting driver v%u.%u.%u\n",
		iorap_miscdev.name, iorap_miscdev.minor,
		IORAP_VERSION >> 16, (IORAP_VERSION >> 8) & 0xff,
		IORAP_VERSION & 0xff);

	return 0;
}
module_init(xiaomi_iorapd_init);

static void __exit xiaomi_iorapd_exit(void)
{
	misc_deregister(&iorap_miscdev);
}
module_exit(xiaomi_iorapd_exit);

MODULE_DESCRIPTION("Dummy /dev/iorap_dev node for Xiaomi launch boost");
MODULE_LICENSE("GPL v2");
