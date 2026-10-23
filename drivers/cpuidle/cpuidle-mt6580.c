// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 standby idle. Cluster/DDR power-down needs the SPM PCM firmware. */
#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>

#include <asm/cpuidle.h>

static struct cpuidle_driver mt6580_idle_driver = {
	.name = "mt6580_idle",
	.owner = THIS_MODULE,
	.states[0] = ARM_CPUIDLE_WFI_STATE,
	/* The s2idle core selects callbacks only above the fallback index 0.
	 * This is the same hardware WFI, with tick/timekeeping frozen by the
	 * core during system sleep. It does not claim SPM or DDR power-down.
	 */
	.states[1] = {
		.enter = arm_cpuidle_simple_enter,
		.enter_s2idle = arm_cpuidle_simple_enter,
		.exit_latency = 1,
		.target_residency = 1,
		.power_usage = UINT_MAX,
		.name = "WFI-STANDBY",
		.desc = "WFI with s2idle tick freeze",
	},
	.state_count = 2,
};

static int __init mt6580_cpuidle_init(void)
{
	if (!of_machine_is_compatible("mediatek,mt6580"))
		return -ENODEV;
	return cpuidle_register(&mt6580_idle_driver, NULL);
}
device_initcall(mt6580_cpuidle_init);
