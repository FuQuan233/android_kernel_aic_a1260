/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_PWRAP_H
#define __LINUX_SOC_MEDIATEK_MT6580_PWRAP_H

struct device;

int mt6580_pwrap_prepare_suspend(struct device *dev);
int mt6580_pwrap_finish_suspend(struct device *dev);

#endif
