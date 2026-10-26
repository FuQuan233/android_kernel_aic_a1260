/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_DDR_H
#define __LINUX_SOC_MEDIATEK_MT6580_DDR_H

#include <linux/types.h>

struct device;

/* Supplier must be bound. Atomic, read-only check of BSP SPM prerequisites. */
int mt6580_ddr_sleep_snapshot(struct device *dev, u32 values[4]);

#endif
