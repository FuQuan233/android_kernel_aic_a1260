/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_USB_H
#define __LINUX_SOC_MEDIATEK_MT6580_USB_H

#include <linux/types.h>

struct device;

/* Query after late suspend with the supplier bound; no MMIO or sleeping. */
bool mt6580_usb_sleep_ready(struct device *dev);

#endif
