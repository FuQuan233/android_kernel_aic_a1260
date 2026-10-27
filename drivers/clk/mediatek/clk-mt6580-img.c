// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 IMGSYS gates from the BSP CG_IMGSYS definitions. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/mt6580-clk.h>
#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs img_regs = {
	.sta_ofs = 0x0, .set_ofs = 0x4, .clr_ofs = 0x8,
};

static const struct mtk_gate img_clks[] = {
	/* The physical upstream routing is not established. COMMON's PM link
	 * retains the verified SMI bus source; no invented CAM/PLL parent.
	 */
	GATE_MTK(CLK_MT6580_IMG_LARB1, "mt6580_smi_larb1", NULL,
		 &img_regs, 0, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_MT6580_IMG_VCODEC, "mt6580_vcodec", NULL,
		 &img_regs, 9, &mtk_clk_gate_ops_setclr),
};

static const struct mtk_clk_desc img_desc = {
	.clks = img_clks,
	.num_clks = ARRAY_SIZE(img_clks),
	.need_runtime_pm = true,
};
static const struct of_device_id mt6580_img_match[] = {
	{ .compatible = "mediatek,mt6580-imgsys", .data = &img_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6580_img_match);
static struct platform_driver mt6580_img_driver = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = { .name = "clk-mt6580-img", .of_match_table = mt6580_img_match },
};
module_platform_driver(mt6580_img_driver);
MODULE_DESCRIPTION("MediaTek MT6580 IMGSYS clocks");
MODULE_LICENSE("GPL");
