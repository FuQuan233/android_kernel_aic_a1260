/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6580_ARCH_PM_H
#define __MT6580_ARCH_PM_H

#include <linux/types.h>

extern u32 mt6580_saved_l2ctlr;
void mt6580_cpu_resume(void);
void mt6580_cache_retention_exit(void);
void mt6580_save_banked(u32 *regs);
void mt6580_restore_banked(u32 *regs);
void mt6580_l2_erratum_entry(void);
int mt6580_cpu0_l2_recover(void);

#endif
