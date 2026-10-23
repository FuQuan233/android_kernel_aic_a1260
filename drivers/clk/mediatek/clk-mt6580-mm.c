// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 display clocks, using the vendor clock manager's AO/gate split. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/mt6580-clk.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs mm1_regs = {
	.sta_ofs = 0x110,
	.set_ofs = 0x114,
	.clr_ofs = 0x118,
};

static const struct mtk_gate mm_clks[] = {
	/* MT6580's BSP treats MMSYS0 clocks as always on. */
	GATE_DUMMY(CLK_MT6580_MM_RDMA0, "mt6580_mm_rdma0"),
	GATE_MTK(CLK_MT6580_MM_DSI_ENGINE, "mt6580_dsi_engine", NULL,
		 &mm1_regs, 2, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_MT6580_MM_DSI_DIGITAL, "mt6580_dsi_digital", NULL,
		 &mm1_regs, 3, &mtk_clk_gate_ops_setclr),
};

static const struct mtk_clk_desc mm_desc = {
	.clks = mm_clks,
	.num_clks = ARRAY_SIZE(mm_clks),
	.need_runtime_pm = true,
};

static const struct platform_device_id mt6580_mm_ids[] = {
	{ "clk-mt6580-mm", (kernel_ulong_t)&mm_desc },
	{ }
};
MODULE_DEVICE_TABLE(platform, mt6580_mm_ids);

static struct platform_driver mt6580_mm_driver = {
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt6580-mm",
	},
	.id_table = mt6580_mm_ids,
};
module_platform_driver(mt6580_mm_driver);
MODULE_DESCRIPTION("MediaTek MT6580 display clocks");
MODULE_LICENSE("GPL");
