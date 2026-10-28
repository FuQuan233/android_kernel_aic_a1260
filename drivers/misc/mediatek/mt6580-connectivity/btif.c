// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 BTIF PIO transport. Register protocol and 16/8-byte FIFO geometry
 * follow the owner's 3.18 BSP btif_plat.c; STP/WMT parsing remains in the
 * original core. No old DMA/APDMA ABI or fabricated completion is used. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "include/mtk_btif_exp.h"
#include "native.h"

#define DATA 0x00
#define IER 0x04
#define IIR_FCR 0x08
#define LCR 0x0c
#define LSR 0x14
#define SLEEP 0x48
#define DMA_EN 0x4c
#define TRIGGER 0x60
#define WAKE 0x64
#define HANDSHAKE 0x6c
#define CLIENT_ID 0x6580

struct native_btif {
	struct device *dev;
	void __iomem *base;
	struct clk *clock;
	int irq;
	struct mutex lock;
	spinlock_t rx_lock;
	DECLARE_KFIFO_PTR(rx, u8);
	struct workqueue_struct *queue;
	struct work_struct work;
	MTK_WCN_BTIF_RX_CB callback;
	bool opened, closing, idle, clock_on;
	u32 saved_ier;
	u64 gates, restores, busy, idle_timeouts, io_restores, wake_signals;
	int last_error;
	unsigned int overflows;
	atomic_t hw_overruns;
};
static struct native_btif *transport;

/* Keep the raw LSR bit1 latch before any read (including TX polling)
 * clears it; distinguish this hardware indication from kfifo overflow. */
static u32 btif_line_status(struct native_btif *b)
{
	u32 status = readl(b->base + LSR);

	if (status & BIT(1))
		atomic_inc(&b->hw_overruns);
	return status;
}

/* b->lock serializes clock votes, process-context MMIO and IRQ depth.
 * The RX interrupt is disabled and synchronized before releasing the clock;
 * its handler never acquires this mutex. Do not flush RX work under this
 * lock: a parser callback may be waiting for it to transmit an STP ACK.
 */
static int btif_activate_locked(struct native_btif *b)
{
	int error;

	lockdep_assert_held(&b->lock);
	if (!b->opened)
		return -ENOTCONN;
	if (b->clock_on)
		return 0;
	error = clk_prepare_enable(b->clock);
	if (error)
		return error;
	WRITE_ONCE(b->clock_on, true);
	writel(b->saved_ier, b->base + IER);
	readl(b->base + IER);
	b->idle = false;
	b->restores++;
	enable_irq(b->irq);
	return 0;
}

static int btif_idle_locked(struct native_btif *b)
{
	u32 status;
	int error;

	lockdep_assert_held(&b->lock);
	if (!b->opened)
		return -ENOTCONN;
	if (b->idle)
		return 0;

	disable_irq(b->irq);
	b->saved_ier = readl(b->base + IER);
	writel(0, b->base + IER);
	readl(b->base + IER);
	error = read_poll_timeout(btif_line_status, status, status & BIT(6),
				  10, 100000, false, b);
	if (error) {
		b->idle_timeouts++;
		goto restore_irq;
	}
	/* A real firmware sleep ACK permits stopping the AP transport, but a
	 * deferred parser/ACK or unread RX byte still owns it. Keep its clock
	 * on in that case. There is no invented successful idle transition.
	 */
	if ((status & BIT(0)) || !kfifo_is_empty(&b->rx) ||
	    work_busy(&b->work)) {
		b->busy++;
		error = -EBUSY;
		goto restore_irq;
	}
	WRITE_ONCE(b->clock_on, false);
	clk_disable_unprepare(b->clock);
	b->idle = true;
	b->gates++;
	return 0;

restore_irq:
	writel(b->saved_ier, b->base + IER);
	readl(b->base + IER);
	enable_irq(b->irq);
	return error;
}

/* Cached transport ownership only; no access to BTIF registers while gated.
 * CCF's clk_summary / TOP CG_CTRL1 bit12 independently expose the HW gate.
 */
static ssize_t btif_state_show(struct device *dev,
			      struct device_attribute *attribute, char *buffer)
{
	struct native_btif *b = transport;
	ssize_t bytes;

	if (!b || b->dev != dev)
		return -ENODEV;
	mutex_lock(&b->lock);
	bytes = sysfs_emit(buffer,
		"opened=%u idle=%u clock_vote=%u gates=%llu restores=%llu busy=%llu idle_timeouts=%llu io_restores=%llu wake_signals=%llu error=%d rx_overflows=%u hw_overruns=%d\n",
		b->opened, b->idle, b->clock_on, b->gates, b->restores,
		b->busy, b->idle_timeouts, b->io_restores, b->wake_signals,
		b->last_error, READ_ONCE(b->overflows), atomic_read(&b->hw_overruns));
	mutex_unlock(&b->lock);
	return bytes;
}
static DEVICE_ATTR_RO(btif_state);

static void btif_receive(struct work_struct *work)
{
	struct native_btif *b = container_of(work, struct native_btif, work);
	u8 bytes[1024];
	unsigned int count;
	MTK_WCN_BTIF_RX_CB callback;
	while ((count = kfifo_out_spinlocked(&b->rx, bytes, sizeof(bytes), &b->rx_lock))) {
		callback = READ_ONCE(b->callback);
		if (callback)
			callback(bytes, count);
		else
			dev_err_ratelimited(b->dev, "BTIF received data without STP callback\n");
	}
}

static irqreturn_t btif_interrupt(int irq, void *data)
{
	struct native_btif *b = data;
	unsigned int count = 0, pending, line_status;
	u8 bytes[256];
	if (!READ_ONCE(b->opened) || !READ_ONCE(b->clock_on))
		return IRQ_NONE;
	pending = readl(b->base + IIR_FCR);
	if (!(pending & (BIT(2) | BIT(6))))
		return IRQ_NONE;
	/* Hardware's RX FIFO is only eight bytes. Drain it in hard IRQ context,
	 * but execute STP callbacks on an ordered process-context queue. */
	while (count < sizeof(bytes)) {
        /* LSR reads clear latched error status; retain actual HW overruns
         * independently of the software receive FIFO's overflow counter. */
        line_status = btif_line_status(b);
        if (!(line_status & BIT(0))) break;
        bytes[count++] = readl(b->base + DATA);
    }
	if (count) {
		unsigned int copied = kfifo_in_spinlocked(&b->rx, bytes, count, &b->rx_lock);
		if (copied != count) {
			b->overflows++;
			dev_err_ratelimited(b->dev, "BTIF RX queue overflow (%u bytes lost)\n", count - copied);
		}
		queue_work(b->queue, &b->work);
	}
	return IRQ_HANDLED;
}

int mt6580_btif_init(struct device *dev, void __iomem *base, struct clk *clock, int irq)
{
	struct native_btif *b;
	int error;
	if (transport)
		return -EBUSY;
	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev; b->base = base; b->clock = clock; b->irq = irq;
	mutex_init(&b->lock); spin_lock_init(&b->rx_lock);
	INIT_WORK(&b->work, btif_receive);
	error = kfifo_alloc(&b->rx, 16384, GFP_KERNEL);
	if (error)
		return error;
	b->queue = alloc_ordered_workqueue("mt6580-stp-rx", WQ_MEM_RECLAIM);
	if (!b->queue) { kfifo_free(&b->rx); return -ENOMEM; }
	error = devm_request_irq(dev, irq, btif_interrupt, IRQF_NO_AUTOEN, "mt6580-btif", b);
	if (error) { destroy_workqueue(b->queue); kfifo_free(&b->rx); return error; }
	transport = b;
	error = device_create_file(dev, &dev_attr_btif_state);
	if (error) {
		transport = NULL;
		destroy_workqueue(b->queue);
		kfifo_free(&b->rx);
		return error;
	}
	return 0;
}

int mtk_wcn_btif_open(char *owner, unsigned long *id)
{
	struct native_btif *b = transport;
	int error;
	if (!b || !owner || !id)
		return -ENODEV;
	mutex_lock(&b->lock);
	if (b->opened || b->closing) { error = -EBUSY; goto out; }
	error = clk_prepare_enable(b->clock);
	if (error)
		goto out;
	WRITE_ONCE(b->clock_on, true);
	kfifo_reset(&b->rx);
	writel(0, b->base + IER);
	writel(0, b->base + LCR);
	writel(1, b->base + HANDSHAKE);
	writel(0, b->base + SLEEP);
	writel(BIT(1), b->base + IIR_FCR); writel(0, b->base + IIR_FCR);
	writel(BIT(2), b->base + IIR_FCR); writel(0, b->base + IIR_FCR);
	writel(8 | BIT(4), b->base + TRIGGER);
	writel(BIT(2), b->base + DMA_EN); /* PIO, with real RX timeout auto reset. */
	WRITE_ONCE(b->opened, true);
	b->idle = false;
	b->saved_ier = BIT(0);
	writel(BIT(0), b->base + IER);
	enable_irq(b->irq);
	*id = CLIENT_ID;
	dev_info(b->dev, "BTIF PIO opened by %s\n", owner);
out:
	mutex_unlock(&b->lock);
	return error;
}

int mtk_wcn_btif_close(unsigned long id)
{
	struct native_btif *b = transport;
	if (!b || id != CLIENT_ID)
		return -EINVAL;
	mutex_lock(&b->lock);
	if (!b->opened) { mutex_unlock(&b->lock); return -ENOTCONN; }
	WRITE_ONCE(b->opened, false);
	b->closing = true;
	if (b->clock_on) {
		writel(0, b->base + IER);
		readl(b->base + IER);
		disable_irq(b->irq);
	}
	/* STP RX may send an ACK. Never wait for it while holding TX lock. */
	mutex_unlock(&b->lock);
	cancel_work_sync(&b->work);
	mutex_lock(&b->lock);
	WRITE_ONCE(b->callback, NULL);
	if (b->clock_on) {
		WRITE_ONCE(b->clock_on, false);
		clk_disable_unprepare(b->clock);
	}
	b->idle = false;
	b->closing = false;
	mutex_unlock(&b->lock);
	return 0;
}

int mtk_wcn_btif_rx_cb_register(unsigned long id, MTK_WCN_BTIF_RX_CB callback)
{
	struct native_btif *b = transport;
	int error = 0;

	if (!b || id != CLIENT_ID)
		return -ENOTCONN;
	mutex_lock(&b->lock);
	if (!b->opened)
		error = -ENOTCONN;
	else
		WRITE_ONCE(b->callback, callback);
	mutex_unlock(&b->lock);
	return error;
}

int mtk_wcn_btif_write(unsigned long id, const unsigned char *bytes, unsigned int length)
{
	struct native_btif *b = transport;
	unsigned int done = 0;
	unsigned long deadline;
	int error;
	if (!b || id != CLIENT_ID || !bytes || !length || length > BTIF_MAX_LEN_PER_PKT)
		return -EINVAL;
	mutex_lock(&b->lock);
	/* Normally PSM restores the transport before sending. Also protect a
	 * late ACK/retry from accessing MMIO after a concurrent idle request.
	 * This restores the AP clock only; it does not fake a firmware wake.
	 */
	if (b->opened && !b->clock_on)
		b->io_restores++;
	error = btif_activate_locked(b);
	if (error) {
		b->last_error = error;
		mutex_unlock(&b->lock);
		return error;
	}
	deadline = jiffies + msecs_to_jiffies(100);
	while (done < length) {
		u32 state = btif_line_status(b);
		unsigned int room = state & BIT(6) ? 16 : state & BIT(5) ? 8 : 0;
		if (!room) {
			if (time_after(jiffies, deadline))
				break;
			usleep_range(5, 20);
			continue;
		}
		room = min(room, length - done);
		while (room--)
			writel(bytes[done++], b->base + DATA);
	}
	mutex_unlock(&b->lock);
	if (!done) {
		dev_err_ratelimited(b->dev, "BTIF TX FIFO timeout\n");
		return -ETIMEDOUT;
	}
	return done;
}

int mtk_wcn_btif_wakeup_consys(unsigned long id)
{
	struct native_btif *b = transport;
	u32 wake;
	int error = 0;

	if (!b || id != CLIENT_ID)
		return -ENOTCONN;
	mutex_lock(&b->lock);
	error = btif_activate_locked(b);
	if (error)
		goto out;
	/* The MT6580 BSP hal_btif_raise_wak_sig() generates a rising edge:
	 * hold AP_WAKEUP_CONSYS low for longer than one 32-kHz period before
	 * raising it. Writing one alone leaves subsequent wake requests high
	 * and does not supply the firmware with another edge. The open BTIF
	 * clock vote and this mutex keep MMIO live throughout the pulse.
	 * Completion here means only that the signal was sent; the WMT core
	 * still requires the real firmware wake event before leaving INACT.
	 */
	wake = readl(b->base + WAKE);
	writel(wake & ~BIT(0), b->base + WAKE);
	readl(b->base + WAKE);
	usleep_range(64, 96);
	writel(wake | BIT(0), b->base + WAKE);
	if (!(readl(b->base + WAKE) & BIT(0)))
		error = -EIO;
	else
		b->wake_signals++;
out:
	b->last_error = error;
	mutex_unlock(&b->lock);
	return error;
}

int mtk_wcn_btif_dpidle_ctrl(unsigned long id, ENUM_BTIF_DPIDLE_CTRL enable)
{
	struct native_btif *b = transport;
	int error;

	if (!b || id != CLIENT_ID || enable < 0 || enable >= BTIF_DPIDLE_MAX)
		return -EINVAL;
	mutex_lock(&b->lock);
	error = enable == BTIF_DPIDLE_ENABLE ? btif_idle_locked(b) :
						btif_activate_locked(b);
	b->last_error = error;
	mutex_unlock(&b->lock);
	return error;
}

int mtk_wcn_btif_loopback_ctrl(unsigned long id, ENUM_BTIF_LPBK_MODE enable)
{
	struct native_btif *b = transport;
	u32 trigger;
	int error;

	if (!b || id != CLIENT_ID || enable >= BTIF_LPBK_MAX)
		return -EINVAL;
	mutex_lock(&b->lock);
	error = btif_activate_locked(b);
	if (!error) {
		trigger = readl(b->base + TRIGGER);
		writel((trigger & ~BIT(7)) | (enable ? BIT(7) : 0), b->base + TRIGGER);
	}
	b->last_error = error;
	mutex_unlock(&b->lock);
	return error;
}

int mtk_wcn_btif_dbg_ctrl(unsigned long id, ENUM_BTIF_DBG_ID flag)
{
	struct native_btif *b = transport;
	int error;

	if (!b || id != CLIENT_ID)
		return -ENOTCONN;
	mutex_lock(&b->lock);
	if (!b->opened) {
		error = -ENOTCONN;
		goto out;
	}
	if (!b->clock_on) {
		error = -EAGAIN;
		goto out;
	}
	if (flag == BTIF_DUMP_BTIF_REG) {
		dev_info(b->dev, "BTIF IIR=%x LSR=%x DMA=%x sleep=%x overflows=%u hw_overruns=%u\n",
			readl(b->base + IIR_FCR), btif_line_status(b), readl(b->base + DMA_EN),
			readl(b->base + SLEEP), b->overflows, atomic_read(&b->hw_overruns));
		error = 0;
	} else {
		error = -EOPNOTSUPP;
	}
out:
	mutex_unlock(&b->lock);
	return error;
}

bool mtk_wcn_btif_parser_wmt_evt(unsigned long id, const char *pattern, unsigned int bytes)
{
	struct native_btif *b = transport;
	u8 data[1024];
	unsigned int length, i;
	unsigned long flags;
	bool found = false;
	if (!b || id != CLIENT_ID || !pattern || !bytes || bytes > sizeof(data))
		return false;
	spin_lock_irqsave(&b->rx_lock, flags);
	length = kfifo_out_peek(&b->rx, data, sizeof(data));
	spin_unlock_irqrestore(&b->rx_lock, flags);
	for (i = 0; i + bytes <= length; i++)
		if (!memcmp(data + i, pattern, bytes)) { found = true; break; }
	return found;
}

int mtk_wcn_btif_read(unsigned long id, unsigned char *bytes, unsigned int length)
{
	return -EOPNOTSUPP; /* All receive ownership belongs to registered STP callback. */
}

void mt6580_btif_exit(void)
{
	struct native_btif *b = transport;
	if (!b)
		return;
	device_remove_file(b->dev, &dev_attr_btif_state);
	if (b->opened)
		mtk_wcn_btif_close(CLIENT_ID);
	transport = NULL;
	destroy_workqueue(b->queue);
	kfifo_free(&b->rx);
}
