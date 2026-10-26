// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6580 non-secure secondary CPU boot, based on the MediaTek 3.18 BSP.
 * Copyright (c) 2015 MediaTek Inc.
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/sizes.h>
#include <linux/smp.h>
#include <linux/soc/mediatek/mt6580-cpu-pm.h>

#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/proc-fns.h>
#include <asm/smp_plat.h>

#include "pm-mt6580.h"

#define MT6580_INFRACFG_AO	0x10001000
#define MT6580_SPM		0x10006000
#define BOOT_ADDR		0x800
#define BOOTROM_PWR_CTRL		0x804
#define SW_ROM_PD		BIT(31)

#define POWERON_CONFIG_EN	0x000
#define SPM_REGWR_EN		((0xb16 << 16) | BIT(0))
#define PWR_STATUS		0x60c
#define PWR_STATUS_2ND		0x610
#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_2ND		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define SRAM_CKISO		BIT(5)
#define SRAM_ISOINT_B		BIT(6)
#define L1_PDN			BIT(0)
#define L1_PDN_ACK		BIT(8)

static void __iomem *mt6580_infracfg;
static void __iomem *mt6580_spm;

static void mt6580_spm_update(unsigned int reg, u32 clear, u32 set)
{
	writel((readl(mt6580_spm + reg) & ~clear) | set,
	       mt6580_spm + reg);
}

static int mt6580_boot_at(unsigned int cpu, u32 vector)
{
	unsigned int id = cpu_logical_map(cpu);
	u32 power, l1, mask, val;
	int ret;

	if (!mt6580_spm || !mt6580_infracfg)
		return -ENODEV;
	if (id < 1 || id > 3)
		return -EINVAL;

	power = 0x218 + (id - 1) * 4;
	l1 = 0x264 + (id - 1) * 8;
	/* SPM CPU1/2/3 status bits are 12/11/10, respectively. */
	mask = BIT(13 - id);

	/* CPU boot requests are serialized by the ARM SMP core. */
	writel(vector, mt6580_infracfg + BOOT_ADDR);
	writel(SPM_REGWR_EN, mt6580_spm + POWERON_CONFIG_EN);
	mt6580_spm_update(power, 0, PWR_ON);
	udelay(1);
	mt6580_spm_update(power, 0, PWR_ON_2ND);
	ret = readl_poll_timeout_atomic(mt6580_spm + PWR_STATUS, val,
					val & mask, 1, 100000);
	if (ret)
		goto timeout;
	ret = readl_poll_timeout_atomic(mt6580_spm + PWR_STATUS_2ND, val,
					val & mask, 1, 100000);
	if (ret)
		goto timeout;

	mt6580_spm_update(power, PWR_ISO, 0);
	mt6580_spm_update(l1, L1_PDN, 0);
	ret = readl_poll_timeout_atomic(mt6580_spm + l1, val,
					!(val & L1_PDN_ACK), 1, 100000);
	if (ret)
		goto timeout;
	udelay(1);
	mt6580_spm_update(power, 0, SRAM_ISOINT_B);
	mt6580_spm_update(power, SRAM_CKISO, 0);
	mt6580_spm_update(power, PWR_CLK_DIS, 0);
	mt6580_spm_update(power, 0, PWR_RST_B);
	/* Complete the reset release before the boot CPU waits for completion. */
	readl(mt6580_spm + power);
	dsb(sy);
	sev();
	return 0;

timeout:
	pr_err("MT6580: CPU%u power-on timed out: power=%#x L1=%#x status=%#x/%#x\n",
	       cpu, readl(mt6580_spm + power), readl(mt6580_spm + l1),
	       readl(mt6580_spm + PWR_STATUS),
	       readl(mt6580_spm + PWR_STATUS_2ND));
	return ret;
}

static int mt6580_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	return mt6580_boot_at(cpu, __pa_symbol(secondary_startup_arm));
}

static void __init mt6580_smp_prepare_cpus(unsigned int max_cpus)
{
	mt6580_infracfg = ioremap(MT6580_INFRACFG_AO, SZ_4K);
	mt6580_spm = ioremap(MT6580_SPM, SZ_4K);
	if (!mt6580_infracfg || !mt6580_spm) {
		pr_err("MT6580: cannot map CPU boot registers\n");
		iounmap(mt6580_infracfg);
		iounmap(mt6580_spm);
		mt6580_infracfg = NULL;
		mt6580_spm = NULL;
		return;
	}

	/* Power-gated cores use the programmable vector instead of Boot ROM. */
	writel(readl(mt6580_infracfg + BOOTROM_PWR_CTRL) | SW_ROM_PD,
	       mt6580_infracfg + BOOTROM_PWR_CTRL);
	writel(__pa_symbol(secondary_startup_arm), mt6580_infracfg + BOOT_ADDR);
}

#ifdef CONFIG_HOTPLUG_CPU
static int mt6580_cpu_disable(unsigned int cpu)
{
	return cpu_logical_map(cpu) ? 0 : -EPERM;
}

static void mt6580_cpu_die(unsigned int cpu)
{
	/* Leave the shared L2 coherent for the other cores before SPM isolation. */
	v7_exit_coherency_flush(louis);
	for (;;)
		cpu_do_idle();
}

static int mt6580_cpu_kill(unsigned int cpu)
{
	unsigned int id = cpu_logical_map(cpu);
	u32 power = 0x218 + (id - 1) * 4;
	u32 l1 = 0x264 + (id - 1) * 8;
	u32 mask = BIT(13 - id), val;
	int ret;

	/* Also used after CPU0 reset, before timekeeping/syscore resume.
	 * Poll atomically: neither sleeping nor ktime_get is available there.
	 * SLEEP_TIMER_STA has one STANDBYWFI bit per A7 core.
	 */
	ret = readl_poll_timeout_atomic(mt6580_spm + 0x720, val,
				val & BIT(16 + id), 10, 100000);
	if (ret)
		goto timeout;
	writel(SPM_REGWR_EN, mt6580_spm + POWERON_CONFIG_EN);
	mt6580_spm_update(power, 0, PWR_ISO | SRAM_CKISO);
	mt6580_spm_update(power, SRAM_ISOINT_B, 0);
	mt6580_spm_update(l1, 0, L1_PDN);
	ret = readl_poll_timeout_atomic(mt6580_spm + l1, val,
				val & L1_PDN_ACK, 1, 100000);
	if (ret)
		goto timeout;
	mt6580_spm_update(power, PWR_RST_B, PWR_CLK_DIS);
	mt6580_spm_update(power, PWR_ON | PWR_ON_2ND, 0);
	ret = readl_poll_timeout_atomic(mt6580_spm + PWR_STATUS, val,
				!(val & mask), 1, 100000);
	if (ret)
		goto timeout;
	ret = readl_poll_timeout_atomic(mt6580_spm + PWR_STATUS_2ND, val,
				!(val & mask), 1, 100000);
	if (!ret)
		return 1;
timeout:
	pr_err("MT6580: CPU%u power-down timed out: WFI=%#x power=%#x L1=%#x\n",
	       cpu, readl(mt6580_spm + 0x720), readl(mt6580_spm + power),
	       readl(mt6580_spm + l1));
	return 0;
}

#ifdef CONFIG_SUSPEND
int mt6580_cpu0_l2_recover(void)
{
	unsigned int cpu, powered = 0, max = min(4U, num_possible_cpus());
	int ret = 0;

	/* BSP erratum 802022: after L2 loss, run each sibling into the small
	 * physical WFI stub before normal Linux secondary boot is permitted.
	 */
	for (cpu = 1; cpu < max; cpu++) {
		ret = mt6580_boot_at(cpu, __pa_symbol(mt6580_l2_erratum_entry));
		if (ret)
			break;
		powered = cpu;
	}
	while (powered) {
		if (!mt6580_cpu_kill(powered))
			ret = -EIO;
		powered--;
	}
	return ret;
}
#endif
#endif

static const struct smp_operations mt6580_smp_ops __initconst = {
	.smp_prepare_cpus = mt6580_smp_prepare_cpus,
	.smp_boot_secondary = mt6580_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_disable = mt6580_cpu_disable,
	.cpu_die = mt6580_cpu_die,
	.cpu_kill = mt6580_cpu_kill,
#endif
};
CPU_METHOD_OF_DECLARE(mt6580_smp, "mediatek,mt6580-smp", &mt6580_smp_ops);
