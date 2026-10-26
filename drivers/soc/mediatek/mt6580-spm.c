// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 PCM loading and PWRAP handoff, retaining all CPU0/DDR state. */
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/soc/mediatek/mt6580-pwrap.h>
#include <linux/suspend.h>

#include <asm/proc-fns.h>

#include "mt6580-spm-pcm.h"

#define PCM_CON0		0x310
#define PCM_CON1		0x314
#define PCM_IM_PTR		0x318
#define PCM_IM_LEN		0x31c
#define PCM_PWR_IO_EN		0x358
#define PCM_FSM_STA		0x3c4
#define PCM_KEY			(0x0b16 << 16)
#define PCM_IM_KICK		BIT(1)
#define PCM_IM_SLEEP_DVS	BIT(3)
#define PCM_SW_RESET		BIT(15)
#define PCM_FSM_DEFAULT		0x48490
#define PCM_FSM_MASK		GENMASK(21, 0)
#define PCM_IM_STATE_MASK	GENMASK(9, 7)
#define PCM_IM_READY		BIT(9)

static const u16 pcm_saved_regs[] = {
	PCM_CON0, PCM_CON1, PCM_IM_PTR, PCM_IM_LEN,
	0x340, 0x344, 0x348, 0x34c,
};

struct mt6580_spm {
	struct device *dev;
	struct device *pwrap;
	void __iomem *base;
	void *pcm;
	dma_addr_t pcm_dma;
	u32 saved[ARRAY_SIZE(pcm_saved_regs)];
	u32 loaded_fsm, restored_fsm;
	unsigned int loads;
	int last_error;
};

/* There is one system suspend provider, bound for the kernel's lifetime. */
static struct mt6580_spm *mt6580_spm;

static int mt6580_pcm_idle(struct mt6580_spm *spm)
{
	if (readl(spm->base + PCM_PWR_IO_EN) ||
	    (readl(spm->base + PCM_FSM_STA) & PCM_FSM_MASK) != PCM_FSM_DEFAULT)
		return -EBUSY;
	return 0;
}

static int mt6580_pcm_reset(struct mt6580_spm *spm)
{
	u32 val;

	writel(PCM_KEY | PCM_SW_RESET, spm->base + PCM_CON0);
	writel(PCM_KEY, spm->base + PCM_CON0);
	return readl_poll_timeout_atomic(spm->base + PCM_FSM_STA, val,
			(val & PCM_FSM_MASK) == PCM_FSM_DEFAULT, 1, 10000);
}

static int mt6580_pcm_load(struct mt6580_spm *spm)
{
	u32 val;
	int i, ret;

	ret = mt6580_pcm_reset(spm);
	if (ret)
		return ret;
	writel(PCM_KEY | PCM_IM_SLEEP_DVS, spm->base + PCM_CON0);
	/* BSP IM fetch: event lock, SRAM retention, non-replace and APB access.
	 * PCM timer/watchdog are off; no PCM_KICK or power-output enable.
	 */
	writel(PCM_KEY | BIT(3) | BIT(6) | BIT(10) | BIT(11) | BIT(12) | BIT(14),
	       spm->base + PCM_CON1);
	writel(lower_32_bits(spm->pcm_dma), spm->base + PCM_IM_PTR);
	writel(ARRAY_SIZE(mt6580_suspend_pcm) - 1, spm->base + PCM_IM_LEN);
	/* Coherent DMA buffer must be visible before starting instruction fetch. */
	dma_wmb();
	writel(PCM_KEY | PCM_IM_SLEEP_DVS | PCM_IM_KICK, spm->base + PCM_CON0);
	writel(PCM_KEY | PCM_IM_SLEEP_DVS, spm->base + PCM_CON0);
	ret = readl_poll_timeout_atomic(spm->base + PCM_FSM_STA, val,
			(val & PCM_IM_STATE_MASK) == PCM_IM_READY, 1, 10000);
	spm->loaded_fsm = val;
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(mt6580_pcm_vectors); i++)
		writel(mt6580_pcm_vectors[i], spm->base + 0x340 + i * 4);
	if (readl(spm->base + PCM_PWR_IO_EN))
		return -EIO;
	spm->loads++;
	return 0;
}

static int mt6580_pcm_restore(struct mt6580_spm *spm)
{
	int i, ret;
	u32 value;

	ret = mt6580_pcm_reset(spm);
	if (ret)
		return ret;
	/* Restore data/vector registers first and controls last. Never replay a
	 * fetch/run/reset command from a sampled control register.
	 */
	for (i = 2; i < ARRAY_SIZE(pcm_saved_regs); i++)
		writel(spm->saved[i], spm->base + pcm_saved_regs[i]);
	value = spm->saved[1] & 0xffff;
	writel(PCM_KEY | value, spm->base + PCM_CON1);
	value = spm->saved[0] & 0xffff & ~(BIT(0) | PCM_IM_KICK | PCM_SW_RESET);
	writel(PCM_KEY | value, spm->base + PCM_CON0);
	spm->restored_fsm = readl(spm->base + PCM_FSM_STA);
	return mt6580_pcm_idle(spm);
}

static int mt6580_spm_prepare_late(void)
{
	struct mt6580_spm *spm = mt6580_spm;
	int ret = mt6580_pcm_idle(spm);

	if (!ret)
		ret = mt6580_pwrap_prepare_suspend(spm->pwrap);
	spm->last_error = ret;
	return ret;
}

static void mt6580_spm_finish(void)
{
	struct mt6580_spm *spm = mt6580_spm;
	int ret = mt6580_pwrap_finish_suspend(spm->pwrap);

	if (ret) {
		spm->last_error = ret;
		dev_err(spm->dev, "PWRAP suspend slots did not restore: %d\n", ret);
	}
}

static int mt6580_spm_enter(suspend_state_t state)
{
	struct mt6580_spm *spm = mt6580_spm;
	int i, ret, restore;

	if (state != PM_SUSPEND_STANDBY || num_online_cpus() != 1 ||
	    smp_processor_id())
		return -EINVAL;
	ret = mt6580_pcm_idle(spm);
	if (ret)
		goto out;
	/* Genpd/device suspend and secondary CPU power-off have completed.
	 * Only PCM registers are touched here, never MTCMOS power controls.
	 */
	for (i = 0; i < ARRAY_SIZE(pcm_saved_regs); i++)
		spm->saved[i] = readl(spm->base + pcm_saved_regs[i]);
	ret = mt6580_pcm_load(spm);
	if (!ret)
		cpu_do_idle();
	restore = mt6580_pcm_restore(spm);
	if (!ret)
		ret = restore;
out:
	spm->last_error = ret;
	return ret;
}

static int mt6580_spm_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_STANDBY;
}

static const struct platform_suspend_ops mt6580_spm_ops = {
	.valid = mt6580_spm_valid,
	.prepare_late = mt6580_spm_prepare_late,
	.enter = mt6580_spm_enter,
	.finish = mt6580_spm_finish,
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "mode=load-only pcm_words=%zu dma=%pad\n"
			 "loads=%u last_error=%d loaded_fsm=%08x restored_fsm=%08x\n"
			 "fsm=%08x con0=%08x con1=%08x pwr_io=%08x\n",
			 ARRAY_SIZE(mt6580_suspend_pcm), &spm->pcm_dma,
			 spm->loads, spm->last_error, spm->loaded_fsm, spm->restored_fsm,
			 readl(spm->base + PCM_FSM_STA), readl(spm->base + PCM_CON0),
			 readl(spm->base + PCM_CON1), readl(spm->base + PCM_PWR_IO_EN));
}
static DEVICE_ATTR_RO(state);

static struct attribute *mt6580_spm_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6580_spm);

static void mt6580_spm_put_device(void *data)
{
	put_device(data);
}

static void mt6580_spm_unmap(void *data)
{
	iounmap(data);
}

static void mt6580_spm_unregister(void *data)
{
	suspend_set_ops(NULL);
	mt6580_spm = NULL;
}

static int mt6580_spm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *pwrap;
	struct device_node *node;
	struct mt6580_spm *spm;
	int ret;

	if (mt6580_spm)
		return -EBUSY;
	if (!of_device_is_compatible(dev->parent->of_node, "mediatek,mt6580-scpsys"))
		return -EINVAL;
	node = of_parse_phandle(dev->of_node, "mediatek,pwrap", 0);
	if (!node)
		return -EINVAL;
	if (!of_device_is_compatible(node, "mediatek,mt6580-firmware-pwrap")) {
		of_node_put(node);
		return -EINVAL;
	}
	pwrap = of_find_device_by_node(node);
	of_node_put(node);
	if (!pwrap)
		return -EPROBE_DEFER;
	if (!device_is_bound(&pwrap->dev)) {
		put_device(&pwrap->dev);
		return -EPROBE_DEFER;
	}
	ret = devm_add_action_or_reset(dev, mt6580_spm_put_device, &pwrap->dev);
	if (ret)
		return ret;
	if (!device_link_add(dev, &pwrap->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	spm = devm_kzalloc(dev, sizeof(*spm), GFP_KERNEL);
	if (!spm)
		return -ENOMEM;
	spm->dev = dev;
	spm->pwrap = &pwrap->dev;
	/* SCPSYS owns the resource. This child shares only its PCM sub-block. */
	spm->base = of_iomap(dev->parent->of_node, 0);
	if (!spm->base)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, mt6580_spm_unmap, spm->base);
	if (ret)
		return ret;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	spm->pcm = dmam_alloc_coherent(dev, sizeof(mt6580_suspend_pcm),
				       &spm->pcm_dma, GFP_KERNEL);
	if (!spm->pcm)
		return -ENOMEM;
	memcpy(spm->pcm, mt6580_suspend_pcm, sizeof(mt6580_suspend_pcm));
	platform_set_drvdata(pdev, spm);
	mt6580_spm = spm;
	suspend_set_ops(&mt6580_spm_ops);
	ret = devm_add_action_or_reset(dev, mt6580_spm_unregister, spm);
	if (ret)
		return ret;
	dev_info(dev, "PCM %s: staged IM fetch only; default s2idle retained\n",
		 MT6580_PCM_VERSION);
	return 0;
}

static const struct of_device_id mt6580_spm_match[] = {
	{ .compatible = "mediatek,mt6580-spm-suspend" },
	{ }
};

static struct platform_driver mt6580_spm_driver = {
	.probe = mt6580_spm_probe,
	.driver = {
		.name = "mt6580-spm-suspend",
		.of_match_table = mt6580_spm_match,
		.dev_groups = mt6580_spm_groups,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6580_spm_driver);
