/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_PWRAP_H
#define __LINUX_SOC_MEDIATEK_MT6580_PWRAP_H

#include <linux/types.h>

struct device;

int mt6580_pwrap_prepare_suspend(struct device *dev);
int mt6580_pwrap_finish_suspend(struct device *dev);
bool mt6580_pwrap_infra_ready(struct device *dev);

#endif
