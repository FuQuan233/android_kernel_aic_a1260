/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_PWRAP_H
#define __LINUX_SOC_MEDIATEK_MT6580_PWRAP_H

#include <linux/types.h>

struct device;

enum mt6580_vproc_sleep_mode {
	MT6580_VPROC_RETAIN,
	MT6580_VPROC_BANK_MARKER,
	MT6580_VPROC_RETENTION_TEST,
	MT6580_VPROC_RETENTION_AUTO,
};

int mt6580_pwrap_prepare_suspend(struct device *dev, enum mt6580_vproc_sleep_mode mode);
int mt6580_pwrap_finish_suspend(struct device *dev);
bool mt6580_pwrap_infra_ready(struct device *dev);

#endif
