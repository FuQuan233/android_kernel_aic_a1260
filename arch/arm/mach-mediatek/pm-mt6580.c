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
#include <asm/memory.h>
#include <asm/irqflags.h>
#include <asm/proc-fns.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/system_misc.h>

#define MT6580_INFRACFG_AO	0x10001000
#define BOOT_ADDR		0x800
#define BOOTROM_PWR_CTRL		0x804
#define SW_ROM_PD		BIT(31)

static void __iomem *mt6580_cpu_boot;

struct mt6580_cpu0_finisher {
	int (*idle)(unsigned long arg);
	unsigned long arg;
};

static int mt6580_cpu0_restart(unsigned long arg)
{
	struct mt6580_cpu0_finisher *finish = (void *)arg;
	int ret = finish->idle(finish->arg);

	if (ret)
		return ret;
	/* Retained-power qualification of the actual MMU-off resume entry.
	 * Use the programmed vector, not a second copy of its expected value.
	 */
	soft_restart(readl(mt6580_cpu_boot + BOOT_ADDR));
	return -EIO;
}

int mt6580_cpu0_resume_entry(struct mt6580_cpu0_context *ctx,
			   int (*idle)(unsigned long), unsigned long arg)
{
	struct mt6580_cpu0_finisher finish = { .idle = idle, .arg = arg };
	unsigned long flags = arch_local_save_flags();
	int ret;

	if (!mt6580_cpu_boot || !irqs_disabled() || num_online_cpus() != 1 ||
	    cpu_logical_map(smp_processor_id()))
		return -EINVAL;
	if (!(readl(mt6580_cpu_boot + BOOTROM_PWR_CTRL) & SW_ROM_PD))
		return -EBUSY;
	ret = cpu_pm_enter();
	if (ret)
		return ret;
	ret = cpu_cluster_pm_enter();
	if (ret)
		goto cpu_exit;
	ctx->boot_before = readl(mt6580_cpu_boot + BOOT_ADDR);
	ctx->boot_resume = __pa_symbol(cpu_resume_arm);
	ctx->counter_before = __arch_counter_get_cntpct();
	ctx->timer_ctrl = arch_timer_reg_read_cp15(ARCH_TIMER_PHYS_ACCESS,
						 ARCH_TIMER_REG_CTRL);
	asm volatile("mrrc p15, 2, %Q0, %R0, c14" : "=r" (ctx->timer_cval));
	writel(ctx->boot_resume, mt6580_cpu_boot + BOOT_ADDR);
	dsb(sy);
	ret = cpu_suspend((unsigned long)&finish, mt6580_cpu0_restart);
	/* Restore the shared vector before CPU1--3 may be brought online. */
	writel(ctx->boot_before, mt6580_cpu_boot + BOOT_ADDR);
	ctx->boot_after = readl(mt6580_cpu_boot + BOOT_ADDR);
	dsb(sy);
	arch_timer_reg_write_cp15(ARCH_TIMER_PHYS_ACCESS, ARCH_TIMER_REG_CVAL,
				 ctx->timer_cval);
	arch_timer_reg_write_cp15(ARCH_TIMER_PHYS_ACCESS, ARCH_TIMER_REG_CTRL,
				 ctx->timer_ctrl & 3);
	ctx->timer_ctrl_after = arch_timer_reg_read_cp15(ARCH_TIMER_PHYS_ACCESS,
						       ARCH_TIMER_REG_CTRL);
	ctx->counter_after = __arch_counter_get_cntpct();
	if (!ret && (ctx->boot_after != ctx->boot_before ||
		     ctx->counter_after < ctx->counter_before ||
		     (ctx->timer_ctrl_after & 3) != (ctx->timer_ctrl & 3)))
		ret = -EIO;
	if (!ret)
		ctx->resumes++;
	ctx->result = ret;
	cpu_cluster_pm_exit();
cpu_exit:
	cpu_pm_exit();
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
		if (!mt6580_cpu_boot)
			return -ENOMEM;
	}
	/* The optional SPM child driver supplies the staged PCM path. */
	if (of_machine_is_compatible("mediatek,mt6580") &&
	    !IS_ENABLED(CONFIG_MTK_MT6580_SPM))
		suspend_set_ops(&mt6580_suspend_ops);
	return 0;
}
late_initcall(mt6580_pm_init);
