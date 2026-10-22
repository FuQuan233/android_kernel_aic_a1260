/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6580 polled command/BTA transport validated against the firmware host. */
static ssize_t mt6580_dsi_transfer(struct mtk_dsi *dsi, const struct mipi_dsi_msg *msg)
{
	const u8 *tx = msg->tx_buf;
	u8 rx[16], type;
	u32 queue[128], start, mode, lc, inten, size, status, word;
	unsigned int i, j, words = 1, len, offset = 1;
	bool read = MTK_DSI_HOST_IS_READ(msg->type);
	int ret;

	if (msg->channel || msg->tx_len > 64 ||
	    (msg->tx_len && !tx) || (read && (!msg->rx_buf || !msg->rx_len)))
		return -EINVAL;
	mtk_dsi_lane_ready(dsi);
	start = readl(dsi->regs + DSI_START);
	mode = readl(dsi->regs + DSI_MODE_CTRL);
	lc = readl(dsi->regs + DSI_PHY_LCCON);
	inten = readl(dsi->regs + DSI_INTEN);
	size = readl(dsi->regs + DSI_CMDQ_SIZE);
	writel(0, dsi->regs + DSI_INTEN);
	synchronize_irq(dsi->irq);
	for (i = 0; i < ARRAY_SIZE(queue); i++)
		queue[i] = readl(dsi->regs + 0x200 + i * 4);
	writel(0, dsi->regs + DSI_START);
	writel(mode & ~MODE, dsi->regs + DSI_MODE_CTRL);
	ret = readl_poll_timeout(dsi->regs + DSI_INTSTA, status,
				!(status & DSI_BUSY), 10, 100000);
	if (ret)
		goto restore;
	writel(lc & ~LC_HS_TX_EN, dsi->regs + DSI_PHY_LCCON);
	word = (msg->type << 8) | (read ? BTA : 0);
	if (msg->tx_len > 2) {
		word |= LONG_PACKET | (msg->tx_len << 16);
		for (i = 0; i < msg->tx_len; i += 4) {
			u32 payload = 0;

			for (j = 0; j < 4 && i + j < msg->tx_len; j++)
				payload |= (u32)tx[i + j] << (8 * j);
			writel(payload, dsi->regs + 0x200 + words++ * 4);
		}
	} else {
		for (i = 0; i < msg->tx_len; i++)
			word |= (u32)tx[i] << (16 + i * 8);
	}
	writel(readl(dsi->regs + DSI_INTSTA) &
	       ~(LPRX_RD_RDY_INT_FLAG | CMD_DONE_INT_FLAG), dsi->regs + DSI_INTSTA);
	writel(word, dsi->regs + 0x200);
	writel(words, dsi->regs + DSI_CMDQ_SIZE);
	writel(1, dsi->regs + DSI_START);
	ret = readl_poll_timeout(dsi->regs + DSI_INTSTA, status,
				status & (read ? LPRX_RD_RDY_INT_FLAG : CMD_DONE_INT_FLAG),
				10, 100000);
	if (read) {
		for (i = 0; i < sizeof(rx); i += 4) {
			u32 val = readl(dsi->regs + DSI_RX_DATA0 + i);

			for (j = 0; j < 4; j++)
				rx[i + j] = val >> (j * 8);
		}
		mtk_dsi_mask(dsi, DSI_INTSTA, LPRX_RD_RDY_INT_FLAG, 0);
		writel(RACK, dsi->regs + DSI_RACK);
	}
	if (ret)
		goto restore;
	ret = readl_poll_timeout(dsi->regs + DSI_INTSTA, status,
				!(status & DSI_BUSY), 10, 100000);
	if (ret)
		goto restore;
	ret = msg->tx_len;
	if (!read)
		goto restore;
	type = rx[0] & 0x3f;
	switch (type) {
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_1BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_1BYTE:
		len = 1;
		break;
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_2BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_2BYTE:
		len = 2;
		break;
	case MIPI_DSI_RX_GENERIC_LONG_READ_RESPONSE:
	case MIPI_DSI_RX_DCS_LONG_READ_RESPONSE:
		len = rx[1] | ((unsigned int)rx[2] << 8);
		offset = 4;
		break;
	default:
		ret = -EPROTO;
		goto restore;
	}
	if (!len || len > sizeof(rx) - offset) {
		ret = -EMSGSIZE;
		goto restore;
	}
	len = min_t(size_t, len, msg->rx_len);
	memcpy(msg->rx_buf, rx + offset, len);
	ret = len;
restore:
	writel(0, dsi->regs + DSI_START);
	if (readl(dsi->regs + DSI_INTSTA) & DSI_BUSY) {
		writel(RACK, dsi->regs + DSI_RACK);
		mtk_dsi_reset_engine(dsi);
	}
	for (i = 0; i < ARRAY_SIZE(queue); i++)
		writel(queue[i], dsi->regs + 0x200 + i * 4);
	writel(size, dsi->regs + DSI_CMDQ_SIZE);
	writel(lc, dsi->regs + DSI_PHY_LCCON);
	mtk_dsi_mask(dsi, DSI_INTSTA, LPRX_RD_RDY_INT_FLAG | CMD_DONE_INT_FLAG, 0);
	writel(mode, dsi->regs + DSI_MODE_CTRL);
	writel(inten, dsi->regs + DSI_INTEN);
	writel(start, dsi->regs + DSI_START);
	return ret;
}
