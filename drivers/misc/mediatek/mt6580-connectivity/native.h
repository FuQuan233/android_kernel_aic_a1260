/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6580_CONSYS_NATIVE_H
#define MT6580_CONSYS_NATIVE_H
#include <linux/device.h>
#include <linux/clk.h>
#include <linux/io.h>
int mt6580_btif_init(struct device *dev, void __iomem *base, struct clk *clock, int irq);
void mt6580_btif_exit(void);
int mt6580_consys_set_vcn33(unsigned int efuse_level);
#endif
