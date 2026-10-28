// SPDX-License-Identifier: GPL-2.0-only
/* Exclusive Android H4 stream over the original MediaTek STP BT task.
 * The legacy stp_chrdev_bt.c supplies the WMT/STP protocol. Keep file I/O,
 * calibration and HCI commands in userspace, with modern file semantics.
 */
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_wakeup.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "stp_exp.h"
#include "wmt_exp.h"

#define BT_STREAM_SIZE 4096

struct bt_stream {
	struct mutex rx_lock, tx_lock;
	u8 rx[BT_STREAM_SIZE], tx[BT_STREAM_SIZE];
};

static atomic_t bt_opened = ATOMIC_INIT(0);
static atomic_t bt_fault = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(bt_rx_wait);
static struct miscdevice bt_misc;
static atomic64_t bt_opens, bt_closes, bt_rx_bytes, bt_tx_bytes, bt_events;

static void bt_event(void)
{
	atomic64_inc(&bt_events);
	if (atomic_read(&bt_opened))
		pm_wakeup_event(bt_misc.this_device, 250);
	wake_up_interruptible_poll(&bt_rx_wait, EPOLLIN | EPOLLRDNORM);
}

static void bt_reset(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
		     ENUM_WMTMSG_TYPE_T type, void *data, unsigned int size)
{
	if (src == WMTDRV_TYPE_WMT && dst == WMTDRV_TYPE_BT &&
	    type == WMTMSG_TYPE_RESET) {
		atomic_set(&bt_fault, 1);
		wake_up_interruptible_poll(&bt_rx_wait, EPOLLERR | EPOLLHUP);
	}
}

static int bt_open(struct inode *inode, struct file *file)
{
	struct bt_stream *stream;
	int error;

	if (atomic_cmpxchg(&bt_opened, 0, 1))
		return -EBUSY;
	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		error = -ENOMEM;
		goto clear;
	}
	mutex_init(&stream->rx_lock);
	mutex_init(&stream->tx_lock);
	atomic_set(&bt_fault, 0);
	if (!mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT)) {
		error = -ENODEV;
		goto free;
	}
	if (!mtk_wcn_stp_is_ready()) {
		error = -ENODEV;
		goto power_off;
	}
	mtk_wcn_stp_set_bluez(0);
	error = mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_BT, bt_reset) ? 0 : -EIO;
	if (error)
		goto power_off;
	error = mtk_wcn_stp_register_event_cb(BT_TASK_INDX, bt_event);
	if (error) {
		mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_BT);
		goto power_off;
	}
	file->private_data = stream;
	atomic64_inc(&bt_opens);
	return nonseekable_open(inode, file);

power_off:
	mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
free:
	kfree(stream);
clear:
	atomic_set(&bt_opened, 0);
	return error;
}

static int bt_release(struct inode *inode, struct file *file)
{
	bool stopped;

	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_BT);
	stopped = mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
	kfree(file->private_data);
	atomic64_inc(&bt_closes);
	atomic_set(&bt_opened, 0);
	return stopped ? 0 : -EIO;
}

static ssize_t bt_read(struct file *file, char __user *buffer, size_t count,
		       loff_t *position)
{
	struct bt_stream *stream = file->private_data;
	int bytes, error;

	if (!count)
		return 0;
	error = mutex_lock_interruptible(&stream->rx_lock);
	if (error)
		return error;
	for (;;) {
		if (atomic_read(&bt_fault)) {
			bytes = -EIO;
			break;
		}
		bytes = mtk_wcn_stp_receive_data(stream->rx,
			min_t(size_t, count, sizeof(stream->rx)), BT_TASK_INDX);
		if (bytes || (file->f_flags & O_NONBLOCK)) {
			if (!bytes)
				bytes = -EAGAIN;
			break;
		}
		bytes = wait_event_interruptible(bt_rx_wait,
			atomic_read(&bt_fault) ||
			!mtk_wcn_stp_is_rxqueue_empty(BT_TASK_INDX));
		if (bytes)
			break;
	}
	if (bytes > 0) {
		if (copy_to_user(buffer, stream->rx, bytes))
			bytes = -EFAULT;
		else
			atomic64_add(bytes, &bt_rx_bytes);
	}
	mutex_unlock(&stream->rx_lock);
	return bytes;
}

static ssize_t bt_write(struct file *file, const char __user *buffer,
			size_t count, loff_t *position)
{
	struct bt_stream *stream = file->private_data;
	int bytes;

	if (!count)
		return 0;
	if (count > sizeof(stream->tx))
		return -EMSGSIZE;
	bytes = mutex_lock_interruptible(&stream->tx_lock);
	if (bytes)
		return bytes;
	if (atomic_read(&bt_fault))
		bytes = -EIO;
	else if (copy_from_user(stream->tx, buffer, count))
		bytes = -EFAULT;
	else {
		bytes = mtk_wcn_stp_send_data(stream->tx, count, BT_TASK_INDX);
		if (!bytes)
			bytes = -EAGAIN;
		if (bytes > 0)
			atomic64_add(bytes, &bt_tx_bytes);
	}
	mutex_unlock(&stream->tx_lock);
	return bytes;
}

static __poll_t bt_poll(struct file *file, poll_table *wait)
{
	__poll_t events = EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &bt_rx_wait, wait);
	if (atomic_read(&bt_fault))
		return EPOLLERR | EPOLLHUP;
	if (!mtk_wcn_stp_is_rxqueue_empty(BT_TASK_INDX))
		events |= EPOLLIN | EPOLLRDNORM;
	return events;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			 char *buffer)
{
	return sysfs_emit(buffer,
		"opened=%d fault=%d opens=%lld closes=%lld rx=%lld tx=%lld events=%lld\n",
		atomic_read(&bt_opened), atomic_read(&bt_fault),
		atomic64_read(&bt_opens), atomic64_read(&bt_closes),
		atomic64_read(&bt_rx_bytes), atomic64_read(&bt_tx_bytes),
		atomic64_read(&bt_events));
}
static DEVICE_ATTR_RO(state);
static struct attribute *bt_attrs[] = { &dev_attr_state.attr, NULL };
ATTRIBUTE_GROUPS(bt);

static const struct file_operations bt_fops = {
	.owner = THIS_MODULE,
	.open = bt_open,
	.release = bt_release,
	.read = bt_read,
	.write = bt_write,
	.poll = bt_poll,
};

static struct miscdevice bt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "stpbt",
	.fops = &bt_fops,
	.groups = bt_groups,
	.mode = 0600,
};

static int __init bt_init(void)
{
	int error = misc_register(&bt_misc);

	if (!error) {
		error = device_init_wakeup(bt_misc.this_device, true);
		if (error)
			misc_deregister(&bt_misc);
	}
	return error;
}
module_init(bt_init);

static void __exit bt_exit(void)
{
	device_init_wakeup(bt_misc.this_device, false);
	misc_deregister(&bt_misc);
}
module_exit(bt_exit);
