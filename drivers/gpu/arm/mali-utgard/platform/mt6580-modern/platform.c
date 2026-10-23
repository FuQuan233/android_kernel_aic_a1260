// SPDX-License-Identifier: GPL-2.0
/* Firmware-retaining MT6580 integration. No DVFS or regulator writes. */
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/dma-mapping.h>

#include "platform_pmm.h"

static struct clk_bulk_data *mali_clocks;
static int mali_num_clocks;

int mali_platform_device_init(struct platform_device *pdev)
{
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	mali_num_clocks = devm_clk_bulk_get_all(&pdev->dev, &mali_clocks);
	if (mali_num_clocks < 0)
		return mali_num_clocks;

	ret = clk_bulk_prepare_enable(mali_num_clocks, mali_clocks);
	if (ret)
		return ret;

	ret = pm_runtime_set_active(&pdev->dev);
	if (ret) {
		clk_bulk_disable_unprepare(mali_num_clocks, mali_clocks);
		return ret;
	}
	pm_runtime_enable(&pdev->dev);
	/* Keep MFG alive while bound; the DDK tracks internal group activation. */
	pm_runtime_get_noresume(&pdev->dev);
	dev_info(&pdev->dev,
		 "Utgard r8p0: retaining firmware clock/voltage, no DVFS\n");
	return 0;
}

int mali_platform_device_deinit(struct platform_device *pdev)
{
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	clk_bulk_disable_unprepare(mali_num_clocks, mali_clocks);
	return 0;
}

void mali_platform_power_mode_change(mali_power_mode mode)
{
	/* Fixed-power platform: clocks and MFG remain held until removal. */
}
