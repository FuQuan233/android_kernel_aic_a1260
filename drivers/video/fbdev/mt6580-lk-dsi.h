/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6580 command queue/BTA polling while retaining LK's PHY configuration. */
#define LK_DSI_START	0x000
#define LK_DSI_INTEN	0x008
#define LK_DSI_INTSTA	0x00c
#define LK_DSI_CON	0x010
#define LK_DSI_MODE	0x014
#define LK_DSI_CMDQ_SIZE	0x060
#define LK_DSI_RX	0x074
#define LK_DSI_RACK	0x084
#define LK_DSI_LCCON	0x104
#define LK_DSI_CMDQ	0x200
#define LK_DSI_RDY	BIT(0)
#define LK_DSI_DONE	BIT(1)
#define LK_DSI_BUSY	BIT(31)

struct lk_dsi_saved {
	u32 start, mode, inten, lc, size;
	u32 queue[128];
};

static void lk_dsi_end(struct lkfb *fb, const struct lk_dsi_saved *saved)
{
	void __iomem *dsi = fb->dsi;
	u32 con;
	unsigned int i;

	writel(0, dsi + LK_DSI_START);
	if (readl(dsi + LK_DSI_INTSTA) & LK_DSI_BUSY) {
		writel(1, dsi + LK_DSI_RACK);
		con = readl(dsi + LK_DSI_CON);
		writel(con | BIT(0), dsi + LK_DSI_CON);
		writel(con, dsi + LK_DSI_CON);
	}
	for (i = 0; i < ARRAY_SIZE(saved->queue); i++)
		writel(saved->queue[i], dsi + LK_DSI_CMDQ + i * 4);
	writel(saved->size, dsi + LK_DSI_CMDQ_SIZE);
	writel(saved->lc, dsi + LK_DSI_LCCON);
	writel(readl(dsi + LK_DSI_INTSTA) & ~(LK_DSI_RDY | LK_DSI_DONE),
	       dsi + LK_DSI_INTSTA);
	writel(saved->mode, dsi + LK_DSI_MODE);
	writel(saved->inten, dsi + LK_DSI_INTEN);
	writel(saved->start, dsi + LK_DSI_START);
}

static int lk_dsi_begin(struct lkfb *fb, struct lk_dsi_saved *saved)
{
	void __iomem *dsi = fb->dsi;
	u32 status;
	unsigned int i;
	int ret;

	saved->start = readl(dsi + LK_DSI_START);
	saved->mode = readl(dsi + LK_DSI_MODE);
	saved->inten = readl(dsi + LK_DSI_INTEN);
	saved->lc = readl(dsi + LK_DSI_LCCON);
	saved->size = readl(dsi + LK_DSI_CMDQ_SIZE);
	for (i = 0; i < ARRAY_SIZE(saved->queue); i++)
		saved->queue[i] = readl(dsi + LK_DSI_CMDQ + i * 4);
	writel(0, dsi + LK_DSI_INTEN);
	writel(0, dsi + LK_DSI_START);
	writel(saved->mode & ~3U, dsi + LK_DSI_MODE);
	ret = readl_poll_timeout(dsi + LK_DSI_INTSTA, status,
				!(status & LK_DSI_BUSY), 10, 100000);
	if (ret) {
		lk_dsi_end(fb, saved);
		return ret;
	}
	writel(saved->lc & ~BIT(0), dsi + LK_DSI_LCCON);
	return 0;
}

static int lk_dsi_read(struct lkfb *fb, u8 command, u8 *data, unsigned int count,
		       u32 rx[4])
{
	void __iomem *dsi = fb->dsi;
	struct lk_dsi_saved saved;
	u32 status;
	u8 bytes[16], type;
	unsigned int i, length, offset;
	int ret;

	if (!count || count > 3)
		return -EINVAL;
	ret = lk_dsi_begin(fb, &saved);
	if (ret)
		return ret;
	writel(readl(dsi + LK_DSI_INTSTA) & ~(LK_DSI_RDY | LK_DSI_DONE),
	       dsi + LK_DSI_INTSTA);
	writel((count << 16) | 0x3700, dsi + LK_DSI_CMDQ);
	writel(((u32)command << 16) | 0x0604, dsi + LK_DSI_CMDQ + 4);
	writel(2, dsi + LK_DSI_CMDQ_SIZE);
	writel(1, dsi + LK_DSI_START);
	ret = readl_poll_timeout(dsi + LK_DSI_INTSTA, status,
				status & LK_DSI_RDY, 10, 100000);
	for (i = 0; i < 4; i++) {
		rx[i] = readl(dsi + LK_DSI_RX + i * 4);
		bytes[i * 4] = rx[i];
		bytes[i * 4 + 1] = rx[i] >> 8;
		bytes[i * 4 + 2] = rx[i] >> 16;
		bytes[i * 4 + 3] = rx[i] >> 24;
	}
	/* Clear read-ready before acknowledging BTA, as in the vendor host. */
	writel(readl(dsi + LK_DSI_INTSTA) & ~LK_DSI_RDY, dsi + LK_DSI_INTSTA);
	writel(1, dsi + LK_DSI_RACK);
	if (ret)
		goto restore;
	ret = readl_poll_timeout(dsi + LK_DSI_INTSTA, status,
				!(status & LK_DSI_BUSY), 10, 100000);
	if (ret)
		goto restore;
	type = bytes[0] & 0x3f;
	offset = 1;
	switch (type) {
	case 0x11: /* Generic short response, one byte. */
	case 0x21: /* DCS short response, one byte. */
		length = 1;
		break;
	case 0x12:
	case 0x22:
		length = 2;
		break;
	case 0x1a:
	case 0x1c:
		length = bytes[1] | ((unsigned int)bytes[2] << 8);
		offset = 4;
		break;
	default:
		ret = -EPROTO;
		goto restore;
	}
	if (!length || length > count || length > sizeof(bytes) - offset) {
		ret = -EMSGSIZE;
		goto restore;
	}
	memcpy(data, bytes + offset, length);
	ret = length;
restore:
	lk_dsi_end(fb, &saved);
	return ret;
}

static int lk_dsi_write(struct lkfb *fb, const u8 *data, unsigned int count)
{
	void __iomem *dsi = fb->dsi;
	u32 type, word, size = 1, status;
	unsigned int i, j;
	int ret;

	if (!count || count > 64)
		return -EINVAL;
	if (data[0] >= 0xb0)
		type = count == 1 ? 0x13 : count == 2 ? 0x23 : 0x29;
	else
		type = count == 1 ? 0x05 : count == 2 ? 0x15 : 0x39;
	if (count <= 2) {
		word = (type << 8) | ((u32)data[0] << 16);
		if (count == 2)
			word |= (u32)data[1] << 24;
	} else {
		word = 2 | (type << 8) | (count << 16);
		for (i = 0; i < count; i += 4) {
			u32 payload = 0;

			for (j = 0; j < 4 && i + j < count; j++)
				payload |= (u32)data[i + j] << (j * 8);
			writel(payload, dsi + LK_DSI_CMDQ + size++ * 4);
		}
	}
	writel(readl(dsi + LK_DSI_INTSTA) & ~LK_DSI_DONE, dsi + LK_DSI_INTSTA);
	writel(word, dsi + LK_DSI_CMDQ);
	writel(size, dsi + LK_DSI_CMDQ_SIZE);
	writel(0, dsi + LK_DSI_START);
	writel(1, dsi + LK_DSI_START);
	ret = readl_poll_timeout(dsi + LK_DSI_INTSTA, status,
				status & LK_DSI_DONE, 10, 100000);
	if (!ret)
		ret = readl_poll_timeout(dsi + LK_DSI_INTSTA, status,
					!(status & LK_DSI_BUSY), 10, 100000);
	return ret;
}

static ssize_t panelid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lkfb *fb = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&fb->dsi_lock);
	len = sysfs_emit(buf, "%s\n", fb->panel_report);
	mutex_unlock(&fb->dsi_lock);
	return len;
}

static ssize_t panelid_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct lkfb *fb = dev_get_drvdata(dev);
	u32 rx[4] = { 0 };
	u8 id[3] = { 0 }, power = 0;
	int id_ret, power_ret, len;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	mutex_lock(&fb->dsi_lock);
	id_ret = lk_dsi_read(fb, 0x04, id, sizeof(id), rx);
	len = scnprintf(fb->panel_report, sizeof(fb->panel_report),
		"DCS 04 result=%d data=%02x %02x %02x rx=%08x %08x %08x %08x\n",
		id_ret, id[0], id[1], id[2], rx[0], rx[1], rx[2], rx[3]);
	power_ret = lk_dsi_read(fb, 0x0a, &power, 1, rx);
	scnprintf(fb->panel_report + len, sizeof(fb->panel_report) - len,
		  "DCS 0a result=%d power=%02x rx=%08x %08x %08x %08x\nVideo configuration restored\n",
		  power_ret, power, rx[0], rx[1], rx[2], rx[3]);
	dev_info(dev, "%s", fb->panel_report);
	mutex_unlock(&fb->dsi_lock);
	return count;
}
static DEVICE_ATTR_RW(panelid);

static ssize_t panel_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lkfb *fb = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&fb->dsi_lock);
	len = sysfs_emit(buf, "%s\n", fb->panel_state);
	mutex_unlock(&fb->dsi_lock);
	return len;
}

static ssize_t panel_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct lkfb *fb = dev_get_drvdata(dev);
	struct lk_dsi_saved saved;
	const u8 on[] = { 0x29, 0x00 }, off[] = { 0x28 };
	bool reinit = sysfs_streq(buf, "reinit");
	bool turn_on = sysfs_streq(buf, "on");
	u8 id[3] = { 0 };
	u32 rx[4], reset;
	unsigned int i;
	int ret;

	if (!reinit && !turn_on && !sysfs_streq(buf, "off"))
		return -EINVAL;
	mutex_lock(&fb->dsi_lock);
	if (reinit) {
		/* Never reset an unidentified panel or apply the wrong table. */
		ret = lk_dsi_read(fb, 0x04, id, sizeof(id), rx);
		if (ret < 2 || id[0] != 0x91 || id[1] != 0x61) {
			ret = -ENODEV;
			goto done;
		}
		if (!(readl(fb->mmsys + 0x150) & BIT(0))) {
			ret = -EOPNOTSUPP;
			goto done;
		}
	}
	ret = lk_dsi_begin(fb, &saved);
	if (ret)
		goto done;
	if (reinit) {
		/* Normal-kernel reset helper: MMSYS LCM_RST_B, bit 0. */
		reset = readl(fb->mmsys + 0x150);
		writel(reset | BIT(0), fb->mmsys + 0x150);
		writel(reset & ~BIT(0), fb->mmsys + 0x150);
		usleep_range(10000, 11000);
		writel(reset | BIT(0), fb->mmsys + 0x150);
		msleep(120);
		for (i = 0; i < ARRAY_SIZE(wt6580_init_9161_cmds); i++) {
			const struct wt6580_panel_cmd *cmd = &wt6580_init_9161_cmds[i];

			if (cmd->delay_ms) {
				msleep(cmd->delay_ms);
				continue;
			}
			ret = lk_dsi_write(fb, cmd->data, cmd->len);
			if (ret)
				goto restore;
		}
	}
	ret = lk_dsi_write(fb, turn_on || reinit ? on : off,
			  turn_on || reinit ? sizeof(on) : sizeof(off));
	if (!ret)
		usleep_range(10000, 11000);
restore:
	lk_dsi_end(fb, &saved);
done:
	scnprintf(fb->panel_state, sizeof(fb->panel_state),
		  "Panel %s: result=%d; DSI video state restored; firmware supplies/PHY retained",
		  reinit ? "reinit" : turn_on ? "on" : "off", ret);
	dev_info(dev, "%s\n", fb->panel_state);
	mutex_unlock(&fb->dsi_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(panel);
