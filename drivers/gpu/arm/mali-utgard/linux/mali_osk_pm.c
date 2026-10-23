/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Fixed-frequency, always-on MT6580 experiment.
 *
 * platform/mt6580-modern/platform.c owns a real clk_bulk enable and runtime-PM
 * reference for the complete bind lifetime. Do not run the old MTK two-workqueue
 * suspend/resume state machine on top of it. The DDK's internal group/domain
 * accounting, activation and real hardware scheduling remain enabled.
 */
#include <linux/atomic.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "mali_kernel_linux.h"
#include "mali_kernel_common.h"
#include "mali_osk.h"

static atomic_t mali_job_pm_refs;

void MTK_mali_osk_pm_dev_enable(void)
{
 atomic_set(&mali_job_pm_refs, 0);
 dev_info(&mali_platform_device->dev,
          "Utgard fixed-power mode: platform owns clocks/MFG; legacy MTK idle suspend disabled\n");
}

void MTK_mali_osk_pm_dev_disable(void)
{
 WARN_ON(atomic_read(&mali_job_pm_refs));
}

_mali_osk_errcode_t _mali_osk_pm_dev_ref_get_sync(void)
{
 if (WARN_ON_ONCE(!pm_runtime_active(&mali_platform_device->dev)))
  return _MALI_OSK_ERR_FAULT;
 atomic_inc(&mali_job_pm_refs);
 return _MALI_OSK_ERR_OK;
}

_mali_osk_errcode_t _mali_osk_pm_dev_ref_get_async(void)
{
 if (WARN_ON_ONCE(!pm_runtime_active(&mali_platform_device->dev)))
  return _MALI_OSK_ERR_FAULT;
 atomic_inc(&mali_job_pm_refs);
 return _MALI_OSK_ERR_OK;
}

void _mali_osk_pm_dev_ref_put(void)
{
 WARN_ON_ONCE(atomic_dec_return(&mali_job_pm_refs) < 0);
}

void _mali_osk_pm_dev_barrier(void)
{
 /* There are no deferred platform-power transitions in fixed-power mode. */
}
