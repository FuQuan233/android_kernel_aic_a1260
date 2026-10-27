/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_IRQ_H
#define __LINUX_SOC_MEDIATEK_MT6580_IRQ_H

#include <linux/types.h>

/* Atomic queries after syscore suspend, before allowing infra power loss. */
bool mt6580_sysirq_suspend_ready(void);
bool mt6580_cirq_suspend_ready(void);

/* Runtime idle: no enabled pending IRQ, and every SPI has a CIRQ route. */
int mt6580_gic_idle_ready(void);
struct mt6580_gic_idle_context {
	u32 enables[8];
	u32 priorities[64];
	u32 pmr;
	unsigned int irq, words;
	bool active;
};
int mt6580_gic_idle_enter(struct mt6580_gic_idle_context *ctx, unsigned int irq);
int mt6580_gic_idle_exit(struct mt6580_gic_idle_context *ctx);
int mt6580_cirq_idle_enter(void);
void mt6580_cirq_idle_exit(void);

#endif
