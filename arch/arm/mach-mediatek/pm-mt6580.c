// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 retained-power standby and CPU0 resume-entry coordination. */
#include <linux/init.h>
#include <linux/cpu_pm.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/smp.h>
#include <linux/soc/mediatek/mt6580-cpu-pm.h>
#include <linux/suspend.h>

#include <asm/arch_timer.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/memory.h>
#include <asm/irqflags.h>
#include <asm/proc-fns.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/system_misc.h>

#include "pm-mt6580.h"

#define MT6580_INFRACFG_AO	0x10001000
#define BOOT_ADDR		0x800
#define BOOTROM_PWR_CTRL		0x804
#define SW_ROM_PD		BIT(31)
#define WDT_MODE_KEY		0x22000000
#define WDT_RESTART_KEY		0x1971
#define WDT_MODE_EN		BIT(0)
#define WDT_MODE_EXRST_EN	BIT(2)
#define WDT_MODE_IRQ_EN		BIT(3)
#define WDT_MODE_DUAL_EN		BIT(6)

static void __iomem *mt6580_cpu_boot;
static void __iomem *mt6580_cpu_config;
static void __iomem *mt6580_cpu_wdt;
u32 mt6580_saved_l2ctlr;

static int mt6580_resume_watchdog_arm(struct mt6580_cpu0_context *ctx)
{
	u32 mode;

	/* Device PM has quiesced watchdogd and the watchdog driver. Keep a
	 * bounded reset fallback for the explicit CPU0 qualification modes.
	 * IRQs are disabled and this is the only online CPU; no driver I/O
	 * can race these temporary writes. Default WFI never uses this path.
	 */
	ctx->wdt_before = readl(mt6580_cpu_wdt);
	ctx->wdt_length = readl(mt6580_cpu_wdt + 4);
	if ((ctx->wdt_before & WDT_MODE_EN) &&
	    (ctx->wdt_before & (WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN)))
		return -EBUSY;
	/* stop() leaves the driver's IRQ/dual selection in the disabled mode.
	 * Use reset-only while timekeeping and IRQ dispatch are suspended, then
	 * restore that exact selection before the driver's resume callback.
	 */
	mode = ctx->wdt_before & ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	mode |= WDT_MODE_EN | WDT_MODE_EXRST_EN;
	writel((30 << 11) | 8, mt6580_cpu_wdt + 4);
	writel(WDT_MODE_KEY | mode, mt6580_cpu_wdt);
	writel(WDT_RESTART_KEY, mt6580_cpu_wdt + 8);
	ctx->wdt_armed = readl(mt6580_cpu_wdt);
	return ctx->wdt_armed & BIT(0) ? 0 : -EIO;
}

static int mt6580_resume_watchdog_restore(struct mt6580_cpu0_context *ctx)
{
	writel(WDT_MODE_KEY | ctx->wdt_before, mt6580_cpu_wdt);
	writel(ctx->wdt_length | 8, mt6580_cpu_wdt + 4);
	writel(WDT_RESTART_KEY, mt6580_cpu_wdt + 8);
	ctx->wdt_after = readl(mt6580_cpu_wdt);
	return ctx->wdt_after == ctx->wdt_before ? 0 : -EIO;
}

struct mt6580_cpu0_finisher {
	int (*idle)(unsigned long arg);
	unsigned long arg;
	unsigned int mode;
};

static int mt6580_cpu0_finish(unsigned long arg)
{
	struct mt6580_cpu0_finisher *finish = (void *)arg;
	u32 sctlr = get_cr(), actlr = get_auxcr();
	int ret;

	if (finish->mode == 2)
		v7_exit_coherency_flush(all);
	ret = finish->idle(finish->arg);
	if (finish->mode == 2) {
		/* WFI returned without a reset: restore coherency/cache state. */
		set_auxcr(actlr);
		dsb(sy);
		set_cr(sctlr);
		return ret ? ret : 1;
	}
	if (ret)
		return ret;
	soft_restart(readl(mt6580_cpu_boot + BOOT_ADDR));
	return -EIO;
}

int mt6580_cpu0_suspend(struct mt6580_cpu0_context *ctx, unsigned int mode,
		       int (*idle)(unsigned long), int (*wait)(unsigned long),
		       unsigned long arg)
{
	struct mt6580_cpu0_finisher finish = { .idle = idle, .arg = arg, .mode = mode };
	unsigned long flags = arch_local_save_flags();
	int ret, status, restore;

	if (!mt6580_cpu_boot || !mt6580_cpu_config || !mt6580_cpu_wdt || !irqs_disabled() ||
	    num_online_cpus() != 1 || cpu_logical_map(smp_processor_id()) ||
	    !mode || mode > 2)
		return -EINVAL;
	if (mode == 2 && !IS_ENABLED(CONFIG_HOTPLUG_CPU))
		return -EOPNOTSUPP;
	if (!(readl(mt6580_cpu_boot + BOOTROM_PWR_CTRL) & SW_ROM_PD))
		return -EBUSY;
	local_fiq_disable();
	ret = cpu_pm_enter();
	if (ret)
		goto fiq_exit;
	ret = cpu_cluster_pm_enter();
	if (ret)
		goto cpu_exit;
	ret = mt6580_resume_watchdog_arm(ctx);
	if (ret) {
		if (ret != -EBUSY)
			mt6580_resume_watchdog_restore(ctx);
		goto cluster_exit;
	}
	ctx->boot_before = readl(mt6580_cpu_boot + BOOT_ADDR);
	ctx->boot_resume = __pa_symbol(mt6580_cpu_resume);
	ctx->counter_before = __arch_counter_get_cntpct();
	ctx->timer_ctrl = arch_timer_reg_read_cp15(ARCH_TIMER_PHYS_ACCESS,
						 ARCH_TIMER_REG_CTRL);
	asm volatile("mrrc p15, 2, %Q0, %R0, c14" : "=r" (ctx->timer_cval));
	asm volatile("mrc p15, 1, %0, c9, c0, 2" : "=r" (ctx->l2ctlr));
	mt6580_saved_l2ctlr = ctx->l2ctlr;
	ctx->cache_config = readl(mt6580_cpu_config);
	writel((ctx->cache_config & ~BIT(4)) | (mode == 1 ? BIT(4) : 0),
	       mt6580_cpu_config);
	mt6580_save_banked(ctx->banked);
	writel(ctx->boot_resume, mt6580_cpu_boot + BOOT_ADDR);
	dsb(sy);
	status = cpu_suspend((unsigned long)&finish, mt6580_cpu0_finish);
	ret = status < 0 ? status : 0;
	if (mode == 2 && !ret && wait)
		ret = wait(arg);
	if (mode == 2 && !status && !ret && IS_ENABLED(CONFIG_HOTPLUG_CPU)) {
		ret = mt6580_cpu0_l2_recover();
		if (!ret)
			ctx->errata_runs++;
	}
	/* Neither the reset vector nor L2 reset policy may leak into hotplug. */
	writel(ctx->boot_before, mt6580_cpu_boot + BOOT_ADDR);
	ctx->boot_after = readl(mt6580_cpu_boot + BOOT_ADDR);
	writel((readl(mt6580_cpu_config) & ~BIT(4)) | (ctx->cache_config & BIT(4)),
	       mt6580_cpu_config);
	ctx->cache_after = readl(mt6580_cpu_config);
	dsb(sy);
	mt6580_restore_banked(ctx->banked);
	arch_timer_reg_write_cp15(ARCH_TIMER_PHYS_ACCESS, ARCH_TIMER_REG_CVAL,
				 ctx->timer_cval);
	arch_timer_reg_write_cp15(ARCH_TIMER_PHYS_ACCESS, ARCH_TIMER_REG_CTRL,
				 ctx->timer_ctrl & 3);
	ctx->timer_ctrl_after = arch_timer_reg_read_cp15(ARCH_TIMER_PHYS_ACCESS,
						       ARCH_TIMER_REG_CTRL);
	ctx->counter_after = __arch_counter_get_cntpct();
	asm volatile("mrc p15, 1, %0, c9, c0, 2" : "=r" (ctx->l2ctlr_after));
	if (!ret && (ctx->boot_after != ctx->boot_before ||
		     ctx->counter_after < ctx->counter_before ||
		     ((ctx->l2ctlr_after ^ ctx->l2ctlr) & GENMASK(17, 0)) ||
		     ((ctx->cache_after ^ ctx->cache_config) & BIT(4)) ||
		     (ctx->timer_ctrl_after & 3) != (ctx->timer_ctrl & 3)))
		ret = -EIO;
	if (!ret && !status) {
		ctx->resumes++;
		if (mode == 2)
			ctx->cold_resumes++;
	} else if (!ret && status == 1) {
		ctx->aborts++;
	}
	restore = mt6580_resume_watchdog_restore(ctx);
	if (!ret)
		ret = restore;
	ctx->result = ret ? ret : status;
cluster_exit:
	cpu_cluster_pm_exit();
cpu_exit:
	cpu_pm_exit();
fiq_exit:
	if (ret)
		ctx->result = ret;
	if (!(flags & PSR_F_BIT))
		local_fiq_enable();
	return ret;
}

static int mt6580_suspend_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_STANDBY;
}

static int mt6580_suspend_enter(suspend_state_t state)
{
	if (state != PM_SUSPEND_STANDBY)
		return -EINVAL;

	/* The suspend core offlines secondary CPUs and runs CIRQ syscore PM.
	 * CPU0 context, GIC, infra and DDR clocks/power remain intact. Do not
	 * invoke cpu_suspend() or reset PCM until their restore paths exist.
	 */
	cpu_do_idle();
	return 0;
}

static const struct platform_suspend_ops mt6580_suspend_ops = {
	.valid = mt6580_suspend_valid,
	.enter = mt6580_suspend_enter,
};

static int __init mt6580_pm_init(void)
{
	if (of_machine_is_compatible("mediatek,mt6580")) {
		mt6580_cpu_boot = ioremap(MT6580_INFRACFG_AO, SZ_4K);
		mt6580_cpu_config = ioremap(0x10200000, SZ_4K);
		mt6580_cpu_wdt = ioremap(0x10007000, SZ_4K);
		if (!mt6580_cpu_boot || !mt6580_cpu_config || !mt6580_cpu_wdt) {
			iounmap(mt6580_cpu_boot);
			iounmap(mt6580_cpu_config);
			iounmap(mt6580_cpu_wdt);
			mt6580_cpu_boot = NULL;
			mt6580_cpu_config = NULL;
			mt6580_cpu_wdt = NULL;
			return -ENOMEM;
		}
	}
	/* The optional SPM child driver supplies the staged PCM path. */
	if (of_machine_is_compatible("mediatek,mt6580") &&
	    !IS_ENABLED(CONFIG_MTK_MT6580_SPM))
		suspend_set_ops(&mt6580_suspend_ops);
	return 0;
}
late_initcall(mt6580_pm_init);
