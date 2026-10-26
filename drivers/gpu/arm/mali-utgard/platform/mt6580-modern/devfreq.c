// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 Utgard DFS: BSP clock steps, shared VPROC retained by CPU OPPs. */
#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>

#include "platform_pmm.h"

static struct devfreq *mali_devfreq;
static struct clk *mali_freq_clock;
static struct regulator *mali_shared_supply;
static DEFINE_SPINLOCK(mali_metrics_lock);
static u64 metrics_start, busy_start, busy_ns;
static unsigned int active_jobs;

/* The DDK accounts jobs from enqueue to completion. Sample that pending
 * or running busy interval independently of its control timer's epoch.
 */
static void mali_utilization_noop(struct mali_gpu_utilization_data *data)
{
}

void mali_platform_gpu_active(void)
{
	unsigned long flags;

	spin_lock_irqsave(&mali_metrics_lock, flags);
	if (!active_jobs++)
		busy_start = ktime_get_ns();
	spin_unlock_irqrestore(&mali_metrics_lock, flags);
}

void mali_platform_gpu_idle(void)
{
	unsigned long flags;

	spin_lock_irqsave(&mali_metrics_lock, flags);
	if (WARN_ON_ONCE(!active_jobs))
		goto out;
	if (!--active_jobs)
		busy_ns += ktime_get_ns() - busy_start;
out:
	spin_unlock_irqrestore(&mali_metrics_lock, flags);
}

static void mali_metrics_reset(void)
{
	unsigned long flags;

	spin_lock_irqsave(&mali_metrics_lock, flags);
	metrics_start = ktime_get_ns();
	busy_start = metrics_start;
	busy_ns = 0;
	spin_unlock_irqrestore(&mali_metrics_lock, flags);
}

static int mali_freq_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct dev_pm_opp *opp;
	int ret, voltage;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	/* All five BSP steps require at least 1.15 V. Never lower the CPU's
	 * shared rail through the GPU OPP API: CPU may currently need 1.3 V.
	 * TOPCKGEN is accessible even when the MFG domain is runtime off.
	 */
	voltage = regulator_get_voltage(mali_shared_supply);
	if (voltage < 0)
		return voltage;
	if (voltage < 1150000)
		return -ERANGE;
	ret = clk_set_rate(mali_freq_clock, *freq);
	if (!ret)
		*freq = clk_get_rate(mali_freq_clock);
	return ret;
}

static int mali_freq_status(struct device *dev, struct devfreq_dev_status *status)
{
	unsigned long flags;
	u64 now, total, busy;

	spin_lock_irqsave(&mali_metrics_lock, flags);
	now = ktime_get_ns();
	busy = busy_ns + (active_jobs ? now - busy_start : 0);
	total = now - metrics_start;
	busy_ns = 0;
	metrics_start = now;
	busy_start = now;
	spin_unlock_irqrestore(&mali_metrics_lock, flags);

	status->current_frequency = clk_get_rate(mali_freq_clock);
	status->total_time = max_t(u64, 1, div_u64(total, NSEC_PER_USEC));
	status->busy_time = div_u64(min(busy, total), NSEC_PER_USEC);
	return 0;
}

static int mali_freq_current(struct device *dev, unsigned long *freq)
{
	*freq = clk_get_rate(mali_freq_clock);
	return 0;
}

static struct devfreq_dev_profile mali_freq_profile = {
	.polling_ms = 100,
	.timer = DEVFREQ_TIMER_DEFERRABLE,
	.target = mali_freq_target,
	.get_dev_status = mali_freq_status,
	.get_cur_freq = mali_freq_current,
};

void mali_platform_devfreq_prepare(void)
{
	unsigned long flags;

	spin_lock_irqsave(&mali_metrics_lock, flags);
	active_jobs = 0;
	spin_unlock_irqrestore(&mali_metrics_lock, flags);
	mali_metrics_reset();
	mali_utilization_callback = mali_utilization_noop;
}

int mali_platform_devfreq_init(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	mali_freq_clock = devm_clk_get(dev, "bus");
	if (IS_ERR(mali_freq_clock))
		return PTR_ERR(mali_freq_clock);
	mali_shared_supply = devm_regulator_get(dev, "mali");
	if (IS_ERR(mali_shared_supply))
		return PTR_ERR(mali_shared_supply);
	ret = dev_pm_opp_of_add_table(dev);
	if (ret)
		return ret;
	mali_freq_profile.initial_freq = clk_get_rate(mali_freq_clock);
	mali_metrics_reset();
	mali_devfreq = devfreq_add_device(dev, &mali_freq_profile,
					DEVFREQ_GOV_SIMPLE_ONDEMAND, NULL);
	if (IS_ERR(mali_devfreq)) {
		ret = PTR_ERR(mali_devfreq);
		mali_devfreq = NULL;
		dev_pm_opp_of_remove_table(dev);
		return ret;
	}
	dev_info(dev, "Utgard devfreq: 107.25..500.5 MHz, shared VPROC unchanged\n");
	return 0;
}

void mali_platform_devfreq_term(struct platform_device *pdev)
{
	if (mali_devfreq) {
		devfreq_remove_device(mali_devfreq);
		mali_devfreq = NULL;
		dev_pm_opp_of_remove_table(&pdev->dev);
	}
}

void mali_platform_devfreq_unprepare(void)
{
	mali_utilization_callback = NULL;
}

int mali_platform_devfreq_suspend(void)
{
	return mali_devfreq ? devfreq_suspend_device(mali_devfreq) : 0;
}

int mali_platform_devfreq_resume(void)
{
	mali_metrics_reset();
	return mali_devfreq ? devfreq_resume_device(mali_devfreq) : 0;
}
