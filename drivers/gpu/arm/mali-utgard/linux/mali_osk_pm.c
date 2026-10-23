/* SPDX-License-Identifier: GPL-2.0 */
/* MT6580 task references feed Linux runtime PM. Async gets run under DDK locks. */
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
}

void MTK_mali_osk_pm_dev_disable(void)
{
	WARN_ON(atomic_read(&mali_job_pm_refs));
}

_mali_osk_errcode_t _mali_osk_pm_dev_ref_get_sync(void)
{
	int ret = pm_runtime_resume_and_get(&mali_platform_device->dev);

	if (ret < 0)
		return _MALI_OSK_ERR_FAULT;
	atomic_inc(&mali_job_pm_refs);
	return _MALI_OSK_ERR_OK;
}

_mali_osk_errcode_t _mali_osk_pm_dev_ref_get_async(void)
{
	int ret;

	/* Never wait for resume while holding the scheduler/executor locks:
	 * the resume callback takes those locks to restart queued jobs.
	 */
	atomic_inc(&mali_job_pm_refs);
	ret = pm_runtime_get(&mali_platform_device->dev);
	if (ret < 0 && ret != -EINPROGRESS) {
		dev_err_ratelimited(&mali_platform_device->dev,
				   "GPU runtime resume request failed: %d\n", ret);
		/* pm_runtime_get took a reference even on error; the caller's
		 * normal put still balances it when the job is aborted.
		 */
		return _MALI_OSK_ERR_FAULT;
	}
	return _MALI_OSK_ERR_OK;
}

void _mali_osk_pm_dev_ref_put(void)
{
	WARN_ON_ONCE(atomic_dec_return(&mali_job_pm_refs) < 0);
	pm_runtime_mark_last_busy(&mali_platform_device->dev);
	pm_runtime_put_autosuspend(&mali_platform_device->dev);
}

void _mali_osk_pm_dev_barrier(void)
{
	pm_runtime_barrier(&mali_platform_device->dev);
}
