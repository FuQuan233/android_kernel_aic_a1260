// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 WFI and guarded CPU0 dormant with L2/infra/DDR retained. */
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/mediatek/mt6580-idle.h>

#include <asm/cpuidle.h>

static __cpuidle int mt6580_enter_dormant(struct cpuidle_device *dev,
					struct cpuidle_driver *drv, int index)
{
	int ret = mt6580_spm_cpu0_idle();

	if (!ret)
		return index;
	if (ret == 1)
		return 0;
	if (ret != -EBUSY && ret != -ENODEV && ret != -EOPNOTSUPP)
		dev->states_usage[index].disable |= CPUIDLE_STATE_DISABLED_BY_DRIVER;
	ct_cpuidle_enter();
	arm_cpuidle_simple_enter(dev, drv, 0);
	ct_cpuidle_exit();
	return 0;
}

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
	.states[2] = {
		.enter = mt6580_enter_dormant,
		.flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_RCU_IDLE,
		.exit_latency = 1000,
		.target_residency = 5000,
		.power_usage = 0,
		.name = "CPU0-DORMANT",
		.desc = "CPU0 off; L2/infra/DDR retained",
	},
	.state_count = IS_ENABLED(CONFIG_MTK_MT6580_SPM) ? 3 : 2,
};

static int mt6580_idle_cpu_prepare(unsigned int cpu)
{
	if (cpu)
		cpuidle_driver_state_disabled(&mt6580_idle_driver, 2, true);
	return 0;
}

static int mt6580_idle_cpu_dead(unsigned int cpu)
{
	/* BP prepare teardown runs after the dying CPU is physically off.
	 * Entry still checks the two power-status banks, IRQs and CPU0 identity.
	 */
	cpuidle_driver_state_disabled(&mt6580_idle_driver, 2,
				      num_online_cpus() != 1 || !cpu_online(0));
	return 0;
}

static int __init mt6580_cpuidle_init(void)
{
	int ret;

	if (!of_machine_is_compatible("mediatek,mt6580"))
		return -ENODEV;
	ret = cpuidle_register(&mt6580_idle_driver, NULL);
	if (ret || !IS_ENABLED(CONFIG_MTK_MT6580_SPM))
		return ret;
	cpuidle_driver_state_disabled(&mt6580_idle_driver, 2, num_online_cpus() != 1);
	ret = cpuhp_setup_state(CPUHP_BP_PREPARE_DYN, "cpuidle/mt6580:prepare",
			       mt6580_idle_cpu_prepare, mt6580_idle_cpu_dead);
	if (ret < 0) {
		cpuidle_unregister(&mt6580_idle_driver);
		return ret;
	}
	return 0;
}
device_initcall(mt6580_cpuidle_init);
