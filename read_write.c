/* read_write.c
 *
 * Linux Character Device Driver — /dev/dummydriver
 *
 * Implements the four fundamental file operations:
 *   open()    → drv_open()
 *   close()   → drv_release()
 *   read()    → drv_read()    — uses copy_to_user()
 *   write()   → drv_write()   — uses copy_from_user()
 *
 * Initialisation sequence (with goto-based rollback on every failure):
 *   1. alloc_chrdev_region()  — dynamic major/minor assignment
 *   2. class_create()         — creates /sys/class/dummydriver_class/
 *   3. device_create()        — triggers udev → /dev/dummydriver
 *   4. cdev_init()            — binds file_operations to the cdev
 *   5. cdev_add()             — registers cdev with the kernel VFS
 *
 * Compatible with Linux kernel >= 6.x (tested on 6.12).
 * Note: class_create() takes a single argument since kernel 6.4.
 */

#include <linux/module.h>   /* MODULE_LICENSE, module_init, module_exit      */
#include <linux/init.h>     /* __init, __exit                                 */
#include <linux/fs.h>       /* file_operations, alloc_chrdev_region,          */
                            /* unregister_chrdev_region, MAJOR, MINOR         */
#include <linux/cdev.h>     /* cdev_init, cdev_add, cdev_del                  */
#include <linux/device.h>   /* class_create, class_destroy,                   */
                            /* device_create, device_destroy                  */
#include <linux/uaccess.h>  /* copy_to_user, copy_from_user                   */
#include <linux/kernel.h>   /* printk, min()                                  */
#include <linux/errno.h>    /* EFAULT, EINVAL, …                              */
#include <linux/string.h>   /* memset                                         */

/* =========================================================================
 * Module metadata
 * ========================================================================= */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("DLH");
MODULE_DESCRIPTION("Character Device Driver — read/write on /dev/dummydriver");
MODULE_VERSION("1.0");

/* =========================================================================
 * Constants
 * ========================================================================= */
#define DRIVER_NAME  "dummydriver"      /* name of the /dev/ node             */
#define DRIVER_CLASS "dummydriver_cls"  /* name visible in /sys/class/        */
#define BUFFER_SIZE  256                /* max bytes the driver can store      */

/* =========================================================================
 * Module-level globals
 * =========================================================================
 *
 * We keep all driver state at file scope (static) so it survives across
 * multiple open/close cycles without dynamic allocation.
 */

/* Device number: encodes both MAJOR (driver id) and MINOR (instance id). */
static dev_t dev_num;

/* cdev: the kernel structure that links a device number to file_operations. */
static struct cdev my_cdev;

/* Device class: groups related devices under /sys/class/DRIVER_CLASS/.      */
static struct class *my_class;

/* Device instance: the concrete entry visible in /sys/devices/ and /dev/.   */
static struct device *my_device;

/* Internal data buffer — shared between read() and write() calls.           */
static char driver_buffer[BUFFER_SIZE];

/* Number of valid bytes currently stored in driver_buffer.                  */
static size_t buffer_len;

/* =========================================================================
 * Forward declarations
 * ========================================================================= */
static int     drv_open   (struct inode *inode, struct file *filp);
static int     drv_release(struct inode *inode, struct file *filp);
static ssize_t drv_read   (struct file *filp, char __user *buf,
                           size_t count, loff_t *f_pos);
static ssize_t drv_write  (struct file *filp, const char __user *buf,
                           size_t count, loff_t *f_pos);

/* =========================================================================
 * file_operations — the VFS dispatch table
 *
 * The kernel calls these function pointers whenever a userspace process
 * invokes the matching syscall on a file descriptor pointing to our device.
 * .owner ensures the module reference count is incremented while the device
 * is held open, preventing rmmod while someone has the fd open.
 * ========================================================================= */
static const struct file_operations fops = {
	.owner   = THIS_MODULE,
	.open    = drv_open,
	.release = drv_release,
	.read    = drv_read,
	.write   = drv_write,
};

/* =========================================================================
 * open() — called when userspace executes open("/dev/dummydriver", flags)
 *
 * inode: contains the device number (imajor/iminor helpers extract it)
 * filp:  the per-open-file structure; we could store private data here
 *        (filp->private_data) for multi-instance drivers.
 *
 * Returning 0 grants access; returning a negative errno denies it.
 * ========================================================================= */
static int drv_open(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "dummydriver: opened (major=%d minor=%d flags=0x%x)\n",
	       imajor(inode), iminor(inode), filp->f_flags);
	return 0;
}

/* =========================================================================
 * release() — called when the last reference to the fd is closed
 *
 * Symmetric with open(). Use it to release per-open resources.
 * A non-zero return is ignored by the VFS for release().
 * ========================================================================= */
static int drv_release(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "dummydriver: closed\n");
	return 0;
}

/* =========================================================================
 * read() — copy driver_buffer content to userspace
 *
 * filp:  the open file (carries f_pos through filp->f_pos, but the VFS
 *        passes it separately as f_pos for convenience)
 * buf:   userspace destination pointer — NEVER dereference directly
 * count: how many bytes userspace wants
 * f_pos: current read offset, updated so sequential reads work correctly
 *
 * Return value (POSIX semantics):
 *   > 0  : number of bytes actually copied
 *   = 0  : EOF — no more data
 *   < 0  : negative errno on error
 *
 * copy_to_user(dst_user, src_kernel, n):
 *   Safely copies n bytes from kernel address src_kernel to the user-space
 *   address dst_user, performing access checks.
 *   Returns the number of bytes that could NOT be copied (0 = full success).
 * ========================================================================= */
static ssize_t drv_read(struct file *filp, char __user *buf,
                        size_t count, loff_t *f_pos)
{
	ssize_t bytes_to_read;
	unsigned long not_copied;

	/* Buffer is empty — return EOF immediately. */
	if (buffer_len == 0) {
		printk(KERN_DEBUG "dummydriver: read() — buffer empty, returning EOF\n");
		return 0;
	}

	/* Caller has already consumed all available data. */
	if (*f_pos >= (loff_t)buffer_len)
		return 0;   /* EOF */

	/*
	 * Clamp: never read beyond the end of the valid data, and never read
	 * more than what the caller requested.
	 */
	bytes_to_read = (ssize_t)min((size_t)(buffer_len - *f_pos), count);

	not_copied = copy_to_user(buf, driver_buffer + *f_pos, bytes_to_read);
	if (not_copied) {
		/*
		 * copy_to_user() returns the residual byte count on partial
		 * failure (e.g. the user pointer became invalid mid-copy).
		 * Report how many bytes were actually transferred.
		 */
		printk(KERN_ERR "dummydriver: read() copy_to_user failed "
		       "(%lu bytes not copied)\n", not_copied);
		return -EFAULT;
	}

	*f_pos += bytes_to_read;

	printk(KERN_INFO "dummydriver: read() → %zd bytes (f_pos now %lld)\n",
	       bytes_to_read, *f_pos);

	return bytes_to_read;
}

/* =========================================================================
 * write() — copy userspace data into driver_buffer
 *
 * buf:   userspace source pointer — NEVER dereference directly
 * count: how many bytes userspace wants to write
 * f_pos: current write offset, updated after the copy
 *
 * We overwrite the buffer from the beginning on each write() call.
 * This mirrors a simple register / mailbox semantics.
 *
 * copy_from_user(dst_kernel, src_user, n):
 *   Safely copies n bytes from user-space address src_user to kernel address
 *   dst_kernel.  Returns the number of bytes NOT copied (0 = full success).
 * ========================================================================= */
static ssize_t drv_write(struct file *filp, const char __user *buf,
                         size_t count, loff_t *f_pos)
{
	ssize_t bytes_to_write;
	unsigned long not_copied;

	if (count == 0)
		return 0;

	/*
	 * Leave one byte for a null-terminator so the buffer is always a
	 * valid C string — useful for debugging with printk %s.
	 */
	bytes_to_write = (ssize_t)min(count, (size_t)(BUFFER_SIZE - 1));

	/* Reset the buffer before writing to avoid stale data. */
	memset(driver_buffer, 0, BUFFER_SIZE);

	not_copied = copy_from_user(driver_buffer, buf, bytes_to_write);
	if (not_copied) {
		printk(KERN_ERR "dummydriver: write() copy_from_user failed "
		       "(%lu bytes not copied)\n", not_copied);
		return -EFAULT;
	}

	/* Explicit null-termination (copy_from_user does not guarantee it). */
	driver_buffer[bytes_to_write] = '\0';
	buffer_len = bytes_to_write;

	*f_pos += bytes_to_write;

	printk(KERN_INFO "dummydriver: write() ← %zd bytes stored: \"%s\"\n",
	       bytes_to_write, driver_buffer);

	return bytes_to_write;
}

/* =========================================================================
 * Module Init — registers the character device
 *
 * Error path uses goto labels so that every successfully completed step is
 * undone in reverse order, leaving the kernel in a clean state.
 * ========================================================================= */
static int __init drv_init(void)
{
	int ret;

	printk(KERN_INFO "dummydriver: loading module\n");

	/* ── 1. Allocate a (major, minor) pair dynamically ───────────────── */
	/*
	 * alloc_chrdev_region(&dev, baseminor, count, name)
	 *   dev       : output — stores the allocated (major, minor)
	 *   baseminor : first minor number to use (0 is conventional)
	 *   count     : how many consecutive minors to reserve (1 here)
	 *   name      : shows up in /proc/devices
	 */
	ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
	if (ret < 0) {
		printk(KERN_ERR "dummydriver: alloc_chrdev_region() failed: %d\n", ret);
		goto err_alloc_region;
	}
	printk(KERN_INFO "dummydriver: device number — major=%d minor=%d\n",
	       MAJOR(dev_num), MINOR(dev_num));

	/* ── 2. Create a device class (/sys/class/dummydriver_cls/) ──────── */
	/*
	 * class_create(name)  [Linux >= 6.4 — single-argument form]
	 * Returns a struct class * or an ERR_PTR on failure.
	 * IS_ERR() / PTR_ERR() are the canonical way to check it.
	 */
	my_class = class_create(DRIVER_CLASS);
	if (IS_ERR(my_class)) {
		ret = PTR_ERR(my_class);
		printk(KERN_ERR "dummydriver: class_create() failed: %d\n", ret);
		goto err_class_create;
	}

	/* ── 3. Create the device node (udev will make /dev/dummydriver) ─── */
	/*
	 * device_create(class, parent, devt, drvdata, fmt, ...)
	 *   class   : the class to attach to
	 *   parent  : parent device (NULL = none)
	 *   devt    : the device number
	 *   drvdata : driver-private data pointer (NULL here)
	 *   fmt     : printf-style name for the node
	 */
	my_device = device_create(my_class, NULL, dev_num, NULL, DRIVER_NAME);
	if (IS_ERR(my_device)) {
		ret = PTR_ERR(my_device);
		printk(KERN_ERR "dummydriver: device_create() failed: %d\n", ret);
		goto err_device_create;
	}

	/* ── 4. Initialise the cdev and bind our file_operations ─────────── */
	/*
	 * cdev_init() zeroes the cdev struct and stores the fops pointer.
	 * Setting .owner = THIS_MODULE prevents the module from being
	 * unloaded while a process holds the device open.
	 */
	cdev_init(&my_cdev, &fops);
	my_cdev.owner = THIS_MODULE;

	/* ── 5. Register the cdev with the VFS ───────────────────────────── */
	/*
	 * cdev_add(cdev, dev, count)
	 *   After this call the device is live: any open() on the major/minor
	 *   will be dispatched to our fops.
	 */
	ret = cdev_add(&my_cdev, dev_num, 1);
	if (ret < 0) {
		printk(KERN_ERR "dummydriver: cdev_add() failed: %d\n", ret);
		goto err_cdev_add;
	}

	/* Initialise buffer state. */
	memset(driver_buffer, 0, BUFFER_SIZE);
	buffer_len = 0;

	printk(KERN_INFO "dummydriver: /dev/%s ready\n", DRIVER_NAME);
	return 0;

	/* ── Error unwinding (strict reverse order) ──────────────────────── */
err_cdev_add:
	device_destroy(my_class, dev_num);
err_device_create:
	class_destroy(my_class);
err_class_create:
	unregister_chrdev_region(dev_num, 1);
err_alloc_region:
	return ret;
}

/* =========================================================================
 * Module Exit — tears down in strict reverse-init order
 *
 * cdev_del    must come before device_destroy (stop accepting new opens
 *             before removing the /dev node).
 * device_destroy comes before class_destroy.
 * class_destroy  comes before unregister_chrdev_region.
 * ========================================================================= */
static void __exit drv_exit(void)
{
	cdev_del(&my_cdev);
	device_destroy(my_class, dev_num);
	class_destroy(my_class);
	unregister_chrdev_region(dev_num, 1);
	printk(KERN_INFO "dummydriver: module unloaded — /dev/%s removed\n", DRIVER_NAME);
}

module_init(drv_init);
module_exit(drv_exit);
