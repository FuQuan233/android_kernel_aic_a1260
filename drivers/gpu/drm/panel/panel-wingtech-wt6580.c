// SPDX-License-Identifier: GPL-2.0-only
/*
 * WT6580 WE N 480x800 panel, recovered from its normal boot backup.
 * See Documentation/arm/mediatek/wt6580-stock-display.rst for provenance.
 * The vendor driver shares one name across IDs 0x9161 and 0x0095.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <video/wt6580-panel-init.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct wt6580_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset;
	struct regulator *power;
	u16 panel_id;
	bool identified;
};

static inline struct wt6580_panel *to_wt6580(struct drm_panel *panel)
{
	return container_of(panel, struct wt6580_panel, panel);
}

/* The vendor DSI host uses generic writes for commands >= 0xb0. */
static void wt6580_init_9161(struct mipi_dsi_multi_context *ctx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(wt6580_init_9161_cmds); i++) {
		const struct wt6580_panel_cmd *cmd = &wt6580_init_9161_cmds[i];

		if (cmd->delay_ms)
			mipi_dsi_msleep(ctx, cmd->delay_ms);
		else if (cmd->data[0] >= 0xb0)
			mipi_dsi_generic_write_multi(ctx, cmd->data, cmd->len);
		else
			mipi_dsi_dcs_write_buffer_multi(ctx, cmd->data, cmd->len);
	}
}

static void wt6580_init_0095(struct mipi_dsi_multi_context *ctx)
{
	mipi_dsi_generic_write_seq_multi(ctx, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_generic_write_seq_multi(ctx, 0xf6, 0x5a, 0x87);
	mipi_dsi_generic_write_seq_multi(ctx, 0xc1, 0x3f);
	mipi_dsi_generic_write_seq_multi(ctx, 0xc2, 0x0e);
	mipi_dsi_generic_write_seq_multi(ctx, 0xc6, 0xf8);
	mipi_dsi_generic_write_seq_multi(ctx, 0xc9, 0x10);
	mipi_dsi_generic_write_seq_multi(ctx, 0xcd, 0x25);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x86, 0x99, 0xa3, 0xa3, 0x71);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x87, 0x04, 0x03, 0x66);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xac, 0x65);
	mipi_dsi_generic_write_seq_multi(ctx, 0xf8, 0x8a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa7, 0x47);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa0, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfa, 0x8f, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x71, 0x48);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x72, 0x48);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x73, 0x00, 0x44);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa3, 0xee);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfd, 0x28, 0x28, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x97, 0xee);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x83, 0x93);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x9a, 0xaf);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x9b, 0x5f);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x82, 0x4f, 0x4f);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb1, 0x10);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7a, 0x13, 0x1a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7b, 0x13, 0x1a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x6d, 0x1e, 0x00, 0x0f, 0x09, 0x01, 0x1f, 0x1e, 0x1e,
		0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e,
		0x1e, 0x1e, 0x1e, 0x1e, 0x1f, 0x08, 0x0a, 0x10, 0x00, 0x1e);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x60, 0x18, 0x0a, 0x7a, 0x7a, 0x18, 0x02, 0x7d, 0x7d);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x63, 0x18, 0x02, 0x6d, 0x6d, 0x18, 0x09, 0x7a, 0x7a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x64, 0x18, 0x09, 0x03, 0x1f, 0x03, 0x03, 0x18, 0x08,
		0x03, 0x20, 0x03, 0x03, 0x7a, 0x7a, 0x7a, 0x7a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x67, 0x18, 0x07, 0x03, 0x21, 0x03, 0x03, 0x18, 0x06,
		0x03, 0x22, 0x03, 0x03, 0x7a, 0x7a, 0x7a, 0x7a);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd1, 0x00, 0x00, 0x00, 0x30, 0x00, 0x60, 0x00, 0x7a,
		0x00, 0xa2, 0x00, 0xca, 0x00, 0xea, 0x01, 0x20, 0x01, 0x50, 0x01, 0x9a, 0x01, 0xdd,
		0x02, 0x35, 0x02, 0x82, 0x02, 0x85, 0x02, 0xc8, 0x03, 0x16, 0x03, 0x40, 0x03, 0x70,
		0x03, 0x9a, 0x03, 0xc0, 0x03, 0xc5, 0x03, 0xca, 0x03, 0xcc, 0x03, 0xd0, 0x03, 0xe1,
		0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd2, 0x00, 0x00, 0x00, 0x30, 0x00, 0x60, 0x00, 0x7a,
		0x00, 0xa2, 0x00, 0xca, 0x00, 0xea, 0x01, 0x20, 0x01, 0x50, 0x01, 0x9a, 0x01, 0xdd,
		0x02, 0x35, 0x02, 0x82, 0x02, 0x85, 0x02, 0xc8, 0x03, 0x16, 0x03, 0x40, 0x03, 0x70,
		0x03, 0x9a, 0x03, 0xc0, 0x03, 0xc5, 0x03, 0xca, 0x03, 0xcc, 0x03, 0xd0, 0x03, 0xe1,
		0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd3, 0x00, 0x00, 0x00, 0x30, 0x00, 0x60, 0x00, 0x7a,
		0x00, 0xa2, 0x00, 0xca, 0x00, 0xea, 0x01, 0x20, 0x01, 0x50, 0x01, 0x9a, 0x01, 0xdd,
		0x02, 0x35, 0x02, 0x82, 0x02, 0x85, 0x02, 0xc8, 0x03, 0x16, 0x03, 0x40, 0x03, 0x70,
		0x03, 0x9a, 0x03, 0xc0, 0x03, 0xc5, 0x03, 0xca, 0x03, 0xcc, 0x03, 0xd0, 0x03, 0xe1,
		0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x60, 0x00, 0x7a,
		0x00, 0xa2, 0x00, 0xca, 0x00, 0xea, 0x01, 0x20, 0x01, 0x50, 0x01, 0x9a, 0x01, 0xdd,
		0x02, 0x35, 0x02, 0x82, 0x02, 0x85, 0x02, 0xc8, 0x03, 0x16, 0x03, 0x40, 0x03, 0x70,
		0x03, 0x9a, 0x03, 0xc0, 0x03, 0xc5, 0x03, 0xca, 0x03, 0xcc, 0x03, 0xd0, 0x03, 0xe1,
		0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd5, 0x00, 0x00, 0x00, 0x30, 0x00, 0x60, 0x00, 0x7a,
		0x00, 0xa2, 0x00, 0xca, 0x00, 0xea, 0x01, 0x20, 0x01, 0x50, 0x01, 0x9a, 0x01, 0xdd,
		0x02, 0x35, 0x02, 0x82, 0x02, 0x85, 0x02, 0xc8, 0x03, 0x16, 0x03, 0x40, 0x03, 0x70,
		0x03, 0x9a, 0x03, 0xc0, 0x03, 0xc5, 0x03, 0xca, 0x03, 0xcc, 0x03, 0xd0, 0x03, 0xe1,
		0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd6, 0x00, 0x00, 0x00, 0x30, 0x00, 0x60, 0x00, 0x7a,
		0x00, 0xa2, 0x00, 0xca, 0x00, 0xea, 0x01, 0x20, 0x01, 0x50, 0x01, 0x9a, 0x01, 0xdd,
		0x02, 0x35, 0x02, 0x82, 0x02, 0x85, 0x02, 0xc8, 0x03, 0x16, 0x03, 0x40, 0x03, 0x70,
		0x03, 0x9a, 0x03, 0xc0, 0x03, 0xc5, 0x03, 0xca, 0x03, 0xcc, 0x03, 0xd0, 0x03, 0xe1,
		0x03, 0xff);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x11, 0x00);
	mipi_dsi_msleep(ctx, 120);
}

/* reset-gpios is active low: logical 1 drives the physical line low. */
static void wt6580_reset(struct wt6580_panel *ctx, bool identifying)
{
	gpiod_set_value_cansleep(ctx->reset, 0);
	if (identifying)
		usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(120);
}

static int wt6580_identify(struct wt6580_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	u8 id[2];
	int ret;

	/* The original compare_id performs a second reset on the first init. */
	wt6580_reset(ctx, true);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbf, 0x91, 0x61, 0xf2);
	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	/* Recovered command queue word 0x00033700 advertises three bytes. */
	ret = mipi_dsi_set_maximum_return_packet_size(ctx->dsi, 3);
	if (ret < 0)
		return ret;
	ret = mipi_dsi_dcs_read(ctx->dsi, 0x04, id, sizeof(id));
	if (ret < 0)
		return ret;
	if (ret != sizeof(id))
		return -EIO;

	ctx->panel_id = (id[0] << 8) | id[1];
	ctx->identified = ctx->panel_id == 0x9161 || ctx->panel_id == 0x0095;
	if (!ctx->identified)
		dev_warn(&ctx->dsi->dev,
			 "Unknown panel ID 0x%04x; using vendor 0x9161 fallback\n",
			 ctx->panel_id);
	else
		dev_info(&ctx->dsi->dev, "Panel ID 0x%04x, selected matching init table\n",
			 ctx->panel_id);

	return 0;
}

static int wt6580_prepare(struct drm_panel *panel)
{
	struct wt6580_panel *ctx = to_wt6580(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	ret = regulator_enable(ctx->power);
	if (ret)
		return ret;

	wt6580_reset(ctx, false);
	if (!ctx->identified) {
		ret = wt6580_identify(ctx);
		if (ret)
			goto power_off;
	}

	if (ctx->identified && ctx->panel_id == 0x0095)
		wt6580_init_0095(&dsi_ctx);
	else
		wt6580_init_9161(&dsi_ctx);
	ret = dsi_ctx.accum_err;
	if (!ret)
		return 0;

power_off:
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->power);
	return ret;
}

static int wt6580_enable(struct drm_panel *panel)
{
	struct wt6580_panel *ctx = to_wt6580(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* Common tail of both recovered initialization tables. */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x29, 0x00);
	mipi_dsi_usleep_range(&dsi_ctx, 10000, 11000);

	return dsi_ctx.accum_err;
}

static int wt6580_disable(struct drm_panel *panel)
{
	struct wt6580_panel *ctx = to_wt6580(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* Preserve zero-parameter short writes from the suspend table. */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x22);
	mipi_dsi_msleep(&dsi_ctx, 100);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x28);
	mipi_dsi_usleep_range(&dsi_ctx, 10000, 11000);

	return dsi_ctx.accum_err;
}

static int wt6580_unprepare(struct drm_panel *panel)
{
	struct wt6580_panel *ctx = to_wt6580(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x10);
	/* Finish power-off even if a DSI transaction failed. */
	msleep(100);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(50);
	ret = regulator_disable(ctx->power);

	return dsi_ctx.accum_err ?: ret;
}

static const struct drm_display_mode wt6580_mode = {
	/* 510 * 816 * 57.43 Hz, inferred from the saved LK fps tag. */
	.clock = 23900,
	.hdisplay = 480,
	.hsync_start = 480 + 10,
	.hsync_end = 480 + 10 + 10,
	.htotal = 480 + 10 + 10 + 10,
	.vdisplay = 800,
	.vsync_start = 800 + 6,
	.vsync_end = 800 + 6 + 4,
	.vtotal = 800 + 6 + 4 + 6,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int wt6580_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &wt6580_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.bpc = 8;

	return 1;
}

static const struct drm_panel_funcs wt6580_funcs = {
	.prepare = wt6580_prepare,
	.enable = wt6580_enable,
	.disable = wt6580_disable,
	.unprepare = wt6580_unprepare,
	.get_modes = wt6580_get_modes,
};

static int wt6580_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct wt6580_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	ctx->power = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->power))
		return dev_err_probe(dev, PTR_ERR(ctx->power), "Failed to get power supply\n");
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "Failed to get reset GPIO\n");

	/*
	 * BSP link rate is twice its recovered 150 MHz PLL setting.
	 * This is a lane-rate limit; the future DSI host must program its PLL.
	 */
	dsi->hs_rate = 300000000;
	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_LPM;
	mipi_dsi_set_drvdata(dsi, ctx);
	drm_panel_init(&ctx->panel, dev, &wt6580_funcs, DRM_MODE_CONNECTOR_DSI);
	/* Keep the DSI clock provider's DISP domain on throughout reset/DCS. */
	ctx->panel.prepare_prev_first = true;
	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;
	drm_panel_add(&ctx->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret)
		drm_panel_remove(&ctx->panel);

	return ret;
}

static void wt6580_shutdown(struct mipi_dsi_device *dsi)
{
	struct wt6580_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (ctx->panel.enabled)
		drm_panel_disable(&ctx->panel);
	if (ctx->panel.prepared)
		drm_panel_unprepare(&ctx->panel);
}

static void wt6580_remove(struct mipi_dsi_device *dsi)
{
	struct wt6580_panel *ctx = mipi_dsi_get_drvdata(dsi);

	wt6580_shutdown(dsi);
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id wt6580_of_match[] = {
	{ .compatible = "wingtech,wt6580-we-n-panel" },
	{ }
};
MODULE_DEVICE_TABLE(of, wt6580_of_match);

static struct mipi_dsi_driver wt6580_driver = {
	.probe = wt6580_probe,
	.remove = wt6580_remove,
	.shutdown = wt6580_shutdown,
	.driver = {
		.name = "panel-wt6580",
		.of_match_table = wt6580_of_match,
	},
};
module_mipi_dsi_driver(wt6580_driver);

MODULE_DESCRIPTION("WT6580 WE N 480x800 DSI panel (IDs 9161/0095)");
MODULE_LICENSE("GPL");
