/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_MEDIATEK_MT6580_CONSYS_H
#define _LINUX_SOC_MEDIATEK_MT6580_CONSYS_H

#include <linux/types.h>

/* Shared provider ownership of TOPRGU's CONSYS MCU reset. Assert before
 * domain power-on; the connectivity driver releases it after clock/ACR setup.
 */
int mt6580_connsys_mcu_reset(bool assert);

#endif
