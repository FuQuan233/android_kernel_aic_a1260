/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MFD_MT6350_USB_H
#define __LINUX_MFD_MT6350_USB_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

#if IS_REACHABLE(CONFIG_EXTCON_MT6350)
/* Cached, non-sleeping query after the detector's normal suspend. */
bool mt6350_usb_suspend_ready(struct device *dev);
#else
static inline bool mt6350_usb_suspend_ready(struct device *dev)
{
	return false;
}
#endif

#endif
