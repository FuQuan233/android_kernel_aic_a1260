/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_IRQ_H
#define __LINUX_SOC_MEDIATEK_MT6580_IRQ_H

#include <linux/types.h>

/* Atomic queries after syscore suspend, before allowing infra power loss. */
bool mt6580_sysirq_suspend_ready(void);
bool mt6580_cirq_suspend_ready(void);

#endif
