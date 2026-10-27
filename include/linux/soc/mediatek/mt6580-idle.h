/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_IDLE_H
#define __LINUX_SOC_MEDIATEK_MT6580_IDLE_H

#include <linux/errno.h>
#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_MTK_MT6580_SPM)
int mt6580_spm_cpu0_idle(void);
#else
static inline int mt6580_spm_cpu0_idle(void) { return -EOPNOTSUPP; }
#endif
#endif
