// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 HACC software-key AES, based on the vendor hacc_sk.c protocol. */
#include <crypto/aes.h>
#include <crypto/engine.h>
#include <crypto/internal/skcipher.h>
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/unaligned.h>

#define HACC_ACON 0x04
#define HACC_ACON2 0x08
#define HACC_ACONK 0x0c
#define HACC_ASRC 0x10
#define HACC_AKEY 0x20
#define HACC_ACFG 0x40
#define HACC_AOUT 0x50
#define HACC_ENC BIT(0)
#define HACC_CBC BIT(1)
#define HACC_START BIT(0)
#define HACC_CLR BIT(1)
#define HACC_READY BIT(15)

struct mt6580_hacc {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct crypto_engine *engine;
	atomic64_t requests, bytes, blocks;
	atomic_t errors, timeouts, resumes, suspends;
	bool broken;
};

struct mt6580_hacc_ctx {
	struct mt6580_hacc *hacc;
	u8 key[AES_MAX_KEY_SIZE];
	unsigned int keylen;
};

struct mt6580_hacc_req {
	u32 mode;
};

/* The DT has one HACC. Module references held by transforms prevent driver
 * unload; manual platform unbind is suppressed. No second physical owner.
 */
static DEFINE_MUTEX(mt6580_hacc_lock);
static struct mt6580_hacc *mt6580_hacc_device;

/* Only the documented software-key/data window is accessed. Never touch
 * CON, SW_OTP, SECINIT, MKJ or select BK2C/R2K hardware-bound key modes.
 */
static void mt6580_hacc_clear(struct mt6580_hacc *h)
{
	unsigned int off;

	writel(HACC_CLR, h->base + HACC_ACON2);
	writel(0, h->base + HACC_ACONK);
	for (off = HACC_ASRC; off < HACC_AOUT; off += 4)
		writel(0, h->base + off);
	writel(0, h->base + HACC_ACON);
	readl(h->base + HACC_ACON);
}

static void mt6580_hacc_setup(struct mt6580_hacc *h, const u8 *key,
			      unsigned int keylen, const u8 *iv, u32 mode)
{
	unsigned int i;

	mt6580_hacc_clear(h);
	/* The BSP reformats its wrapping keys. Raw Linux AES keys, data and
	 * IV use little-endian words with byte swapping disabled (ACON[12]=0).
	 * A known-answer test verifies this before any algorithms are exposed.
	 */
	for (i = 0; i < keylen; i += 4)
		writel(get_unaligned_le32(key + i), h->base + HACC_AKEY + i);
	if (iv)
		for (i = 0; i < AES_BLOCK_SIZE; i += 4)
			writel(get_unaligned_le32(iv + i), h->base + HACC_ACFG + i);
	writel(mode | ((keylen - AES_KEYSIZE_128) / 8 << 4), h->base + HACC_ACON);
}

static int mt6580_hacc_block(struct mt6580_hacc *h, const u8 *src, u8 *dst)
{
	unsigned int i;
	u32 status;
	int ret;

	for (i = 0; i < AES_BLOCK_SIZE; i += 4)
		writel(get_unaligned_le32(src + i), h->base + HACC_ASRC + i);
	writel(HACC_START, h->base + HACC_ACON2);
	/* The BSP spins forever. Do not fabricate data or replay a timed-out
	 * request; latch the engine unavailable until reprobe/reboot instead.
	 */
	ret = readl_poll_timeout_atomic(h->base + HACC_ACON2, status,
					status & HACC_READY, 1, 1000);
	if (ret) {
		atomic_inc(&h->timeouts);
		WRITE_ONCE(h->broken, true);
		return ret;
	}
	for (i = 0; i < AES_BLOCK_SIZE; i += 4)
		put_unaligned_le32(readl(h->base + HACC_AOUT + i), dst + i);
	atomic64_inc(&h->blocks);
	return 0;
}

static int mt6580_hacc_kat(struct mt6580_hacc *h)
{
	static const u8 plain[AES_BLOCK_SIZE] = {
		0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
		0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
	};
	static const u8 cipher[AES_BLOCK_SIZE] = {
		0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
		0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a,
	};
	u8 key[AES_KEYSIZE_128], out[AES_BLOCK_SIZE];
	unsigned int i;
	int ret;

	for (i = 0; i < sizeof(key); i++)
		key[i] = i;
	mt6580_hacc_setup(h, key, sizeof(key), NULL, HACC_ENC);
	ret = mt6580_hacc_block(h, plain, out);
	if (!ret && memcmp(out, cipher, sizeof(out)))
		ret = -EIO;
	if (!ret) {
		mt6580_hacc_setup(h, key, sizeof(key), NULL, 0);
		ret = mt6580_hacc_block(h, cipher, out);
		if (!ret && memcmp(out, plain, sizeof(out)))
			ret = -EIO;
	}
	mt6580_hacc_clear(h);
	memzero_explicit(key, sizeof(key));
	memzero_explicit(out, sizeof(out));
	return ret;
}

static int mt6580_hacc_one_request(struct crypto_engine *engine, void *areq)
{
	struct skcipher_request *req = skcipher_request_cast(areq);
	struct mt6580_hacc_ctx *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	struct mt6580_hacc_req *rctx = skcipher_request_ctx(req);
	struct mt6580_hacc *h = ctx->hacc;
	struct skcipher_walk walk;
	u8 input[AES_BLOCK_SIZE];
	unsigned int done, count;
	int ret;

	if (READ_ONCE(h->broken)) {
		ret = -EIO;
		goto finish;
	}
	ret = pm_runtime_resume_and_get(h->dev);
	if (ret < 0)
		goto finish;
	ret = skcipher_walk_virt(&walk, req, false);
	if (ret)
		goto clear;
	mt6580_hacc_setup(h, ctx->key, ctx->keylen,
			  rctx->mode & HACC_CBC ? walk.iv : NULL, rctx->mode);
	while (walk.nbytes) {
		count = round_down(walk.nbytes, AES_BLOCK_SIZE);
		for (done = 0; done < count; done += AES_BLOCK_SIZE) {
			const u8 *src = walk.src.virt.addr + done;
			u8 *dst = walk.dst.virt.addr + done;

			/* Save ciphertext before an in-place CBC decryption. */
			memcpy(input, src, AES_BLOCK_SIZE);
			ret = mt6580_hacc_block(h, src, dst);
			if (ret)
				break;
			if (rctx->mode & HACC_CBC)
				memcpy(walk.iv, rctx->mode & HACC_ENC ? dst : input,
				       AES_BLOCK_SIZE);
		}
		/* walk_done may copy its whole bounce buffer even on error. Do
		 * not expose uninitialized tail bytes after a hardware failure.
		 */
		if (ret)
			memzero_explicit(walk.dst.virt.addr, walk.nbytes);
		ret = skcipher_walk_done(&walk, ret ?: walk.nbytes - count);
		if (ret)
			break;
		cond_resched();
	}
	if (!ret)
		atomic64_add(req->cryptlen, &h->bytes);
clear:
	mt6580_hacc_clear(h);
	memzero_explicit(input, sizeof(input));
	pm_runtime_mark_last_busy(h->dev);
	pm_runtime_put_autosuspend(h->dev);
finish:
	atomic64_inc(&h->requests);
	if (ret)
		atomic_inc(&h->errors);
	crypto_finalize_skcipher_request(engine, req, ret);
	return 0;
}

static int mt6580_hacc_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int len)
{
	struct mt6580_hacc_ctx *ctx = crypto_skcipher_ctx(tfm);

	memzero_explicit(ctx->key, sizeof(ctx->key));
	ctx->keylen = 0;
	if (len != AES_KEYSIZE_128 && len != AES_KEYSIZE_192 && len != AES_KEYSIZE_256)
		return -EINVAL;
	memcpy(ctx->key, key, len);
	ctx->keylen = len;
	return 0;
}

static int mt6580_hacc_init_tfm(struct crypto_skcipher *tfm)
{
	struct mt6580_hacc_ctx *ctx = crypto_skcipher_ctx(tfm);

	ctx->hacc = READ_ONCE(mt6580_hacc_device);
	if (!ctx->hacc)
		return -ENODEV;
	crypto_skcipher_set_reqsize(tfm, sizeof(struct mt6580_hacc_req));
	return 0;
}

static void mt6580_hacc_exit_tfm(struct crypto_skcipher *tfm)
{
	memzero_explicit(crypto_skcipher_ctx(tfm), sizeof(struct mt6580_hacc_ctx));
}

static int mt6580_hacc_crypt(struct skcipher_request *req, u32 mode)
{
	struct mt6580_hacc_ctx *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	struct mt6580_hacc_req *rctx = skcipher_request_ctx(req);

	if (req->cryptlen % AES_BLOCK_SIZE)
		return -EINVAL;
	if (!req->cryptlen)
		return 0;
	if (!ctx->keylen)
		return -ENOKEY;
	if (READ_ONCE(ctx->hacc->broken))
		return -EIO;
	rctx->mode = mode;
	return crypto_transfer_skcipher_request_to_engine(ctx->hacc->engine, req);
}

static int mt6580_hacc_ecb_enc(struct skcipher_request *r) { return mt6580_hacc_crypt(r, HACC_ENC); }
static int mt6580_hacc_ecb_dec(struct skcipher_request *r) { return mt6580_hacc_crypt(r, 0); }
static int mt6580_hacc_cbc_enc(struct skcipher_request *r) { return mt6580_hacc_crypt(r, HACC_CBC | HACC_ENC); }
static int mt6580_hacc_cbc_dec(struct skcipher_request *r) { return mt6580_hacc_crypt(r, HACC_CBC); }

#define HACC_ALG(mode, ivbytes) { \
	.base = { \
		.base.cra_name = #mode "(aes)", \
		.base.cra_driver_name = "mt6580-" #mode "-aes", \
		.base.cra_priority = 50, \
		.base.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_KERN_DRIVER_ONLY, \
		.base.cra_blocksize = AES_BLOCK_SIZE, \
		.base.cra_ctxsize = sizeof(struct mt6580_hacc_ctx), \
		.base.cra_module = THIS_MODULE, \
		.min_keysize = AES_MIN_KEY_SIZE, .max_keysize = AES_MAX_KEY_SIZE, \
		.ivsize = ivbytes, .setkey = mt6580_hacc_setkey, \
		.init = mt6580_hacc_init_tfm, .exit = mt6580_hacc_exit_tfm, \
		.encrypt = mt6580_hacc_##mode##_enc, .decrypt = mt6580_hacc_##mode##_dec, \
	}, \
	.op.do_one_request = mt6580_hacc_one_request, \
}
static struct skcipher_engine_alg mt6580_hacc_algs[] = {
	HACC_ALG(ecb, 0), HACC_ALG(cbc, AES_BLOCK_SIZE),
};

static int mt6580_hacc_runtime_resume(struct device *dev)
{
	struct mt6580_hacc *h = dev_get_drvdata(dev);
	int ret = clk_prepare_enable(h->clk);

	if (!ret) {
		mt6580_hacc_clear(h);
		atomic_inc(&h->resumes);
	}
	return ret;
}

static int mt6580_hacc_runtime_suspend(struct device *dev)
{
	struct mt6580_hacc *h = dev_get_drvdata(dev);

	mt6580_hacc_clear(h);
	clk_disable_unprepare(h->clk);
	atomic_inc(&h->suspends);
	return 0;
}

static int mt6580_hacc_suspend(struct device *dev)
{
	struct mt6580_hacc *h = dev_get_drvdata(dev);
	int ret = crypto_engine_stop(h->engine);

	if (ret)
		return ret;
	ret = pm_runtime_force_suspend(dev);
	if (ret)
		crypto_engine_start(h->engine);
	return ret;
}

static int mt6580_hacc_resume(struct device *dev)
{
	struct mt6580_hacc *h = dev_get_drvdata(dev);
	int ret = pm_runtime_force_resume(dev);

	return ret ?: crypto_engine_start(h->engine);
}

static const struct dev_pm_ops mt6580_hacc_pm = {
	RUNTIME_PM_OPS(mt6580_hacc_runtime_suspend, mt6580_hacc_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(mt6580_hacc_suspend, mt6580_hacc_resume)
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_hacc *h = dev_get_drvdata(dev);

	return sysfs_emit(buf, "broken=%u requests=%lld bytes=%lld blocks=%lld errors=%d timeouts=%d\n"
		"pm resumes=%d suspends=%d\n", READ_ONCE(h->broken),
		atomic64_read(&h->requests), atomic64_read(&h->bytes), atomic64_read(&h->blocks),
		atomic_read(&h->errors), atomic_read(&h->timeouts),
		atomic_read(&h->resumes), atomic_read(&h->suspends));
}
static DEVICE_ATTR_RO(state);
static struct attribute *mt6580_hacc_attrs[] = { &dev_attr_state.attr, NULL };
ATTRIBUTE_GROUPS(mt6580_hacc);

static int mt6580_hacc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6580_hacc *h;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	h->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(h->base))
		return PTR_ERR(h->base);
	h->clk = devm_clk_get(dev, "bus");
	if (IS_ERR(h->clk))
		return dev_err_probe(dev, PTR_ERR(h->clk), "SEJ bus clock unavailable\n");
	platform_set_drvdata(pdev, h);
	pm_runtime_set_autosuspend_delay(dev, 100);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto disable_pm;
	ret = mt6580_hacc_kat(h);
	pm_runtime_put_sync_suspend(dev);
	if (ret) {
		dev_err(dev, "AES known-answer test failed: %d\n", ret);
		goto disable_pm;
	}
	h->engine = crypto_engine_alloc_init(dev, false);
	if (!h->engine) {
		ret = -ENOMEM;
		goto disable_pm;
	}
	ret = crypto_engine_start(h->engine);
	if (ret)
		goto exit_engine;
	mutex_lock(&mt6580_hacc_lock);
	if (mt6580_hacc_device) {
		ret = -EBUSY;
	} else {
		WRITE_ONCE(mt6580_hacc_device, h);
		ret = crypto_engine_register_skciphers(mt6580_hacc_algs, ARRAY_SIZE(mt6580_hacc_algs));
		if (ret)
			WRITE_ONCE(mt6580_hacc_device, NULL);
	}
	mutex_unlock(&mt6580_hacc_lock);
	if (ret)
		goto exit_engine;
	dev_info(dev, "software-key AES ECB/CBC registered; hardware KAT passed\n");
	return 0;
exit_engine:
	crypto_engine_exit(h->engine);
disable_pm:
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		mt6580_hacc_runtime_suspend(dev);
	return ret;
}

static void mt6580_hacc_remove(struct platform_device *pdev)
{
	struct mt6580_hacc *h = platform_get_drvdata(pdev);

	crypto_engine_unregister_skciphers(mt6580_hacc_algs, ARRAY_SIZE(mt6580_hacc_algs));
	WRITE_ONCE(mt6580_hacc_device, NULL);
	crypto_engine_exit(h->engine);
	pm_runtime_disable(h->dev);
	if (!pm_runtime_status_suspended(h->dev))
		mt6580_hacc_runtime_suspend(h->dev);
}

static void mt6580_hacc_shutdown(struct platform_device *pdev)
{
	struct mt6580_hacc *h = platform_get_drvdata(pdev);
	int ret = crypto_engine_stop(h->engine);

	if (!ret)
		ret = pm_runtime_force_suspend(h->dev);
	if (ret)
		dev_err(h->dev, "shutdown could not quiesce AES: %d\n", ret);
}

static const struct of_device_id mt6580_hacc_of_match[] = {
	{ .compatible = "mediatek,mt6580-hacc" }, { }
};
MODULE_DEVICE_TABLE(of, mt6580_hacc_of_match);
static struct platform_driver mt6580_hacc_driver = {
	.probe = mt6580_hacc_probe,
	.remove = mt6580_hacc_remove,
	.shutdown = mt6580_hacc_shutdown,
	.driver = {
		.name = "mt6580-hacc", .of_match_table = mt6580_hacc_of_match,
		.pm = pm_ptr(&mt6580_hacc_pm), .dev_groups = mt6580_hacc_groups,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6580_hacc_driver);
MODULE_DESCRIPTION("MT6580 HACC software-key AES engine");
MODULE_LICENSE("GPL");
