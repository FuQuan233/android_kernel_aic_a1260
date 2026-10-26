// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 retained-state standby, before SPM/PCM deep-sleep integration. */
#include <linux/init.h>
#include <linux/of.h>
#include <linux/suspend.h>

#include <asm/proc-fns.h>

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
	if (of_machine_is_compatible("mediatek,mt6580"))
		suspend_set_ops(&mt6580_suspend_ops);
	return 0;
}
late_initcall(mt6580_pm_init);
