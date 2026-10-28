// SPDX-License-Identifier: GPL-2.0-only
/* Native MT6580 platform for the GPL WMT/STP core.
 * The owner's wmt_drv.ko has no CONNMCU_BUS gate operation. CONSYS power and
 * MCU reset are owned by SCPSYS and the shared TOPRGU reset provider; BTIF
 * alone consumes its actual TOP CG_CTRL1 bit12 clock. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/mt6350/registers.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/soc/mediatek/mt6580-consys.h>
#include "include/wmt_plat.h"
#include "wmt_lib.h"
#include "native.h"
#include "include/mtk_btif_exp.h"

#define DUMP_OFFSET SZ_512K
#define DUMP_SIZE (343 * SZ_1K)
#define EMI_REGION5 0x188
#define EMI_REGION6 0x190
#define EMI_PERMISSIONS45 0x1b0
#define EMI_PERMISSIONS67 0x1b8
#define EMI_VIOLATION 0x1f0
#define EMI_VIOLATION_ADDRESS 0x1f8
#define EMI_CONN_PERMISSION 0x0a28
#define EMI_WIFI_PERMISSION 0x0a2d
struct native_consys {
	struct device *dev;
	void __iomem *mcu, *top, *gpio, *spm, *emi, *mpu;
	phys_addr_t emi_base;
	struct regulator *v18, *v28, *v33_wifi, *v33_bt;
	struct mutex lock;
	spinlock_t mpu_lock;
	struct wakeup_source *work_lock;
	irq_cb bgf_callback;
	int bgf_irq;
	bool on, wifi_ldo, bt_ldo, v28_on, bgf_wake;
	atomic_t bgf_enabled;
	u32 chipid;
	int vcn33_uv;
	unsigned int efuse_level;
	int last_error;
	u64 power_ons, power_offs;
};
static struct native_consys *con;
static irq_cb registered_bgf_callback;
static device_audio_if_cb audio_callback;
static func_ctrl_cb function_callback;
static thermal_query_ctrl_cb thermal_callback;
static deep_idle_ctrl_cb deep_callback;
UINT32 wmtPlatLogLvl = 1;
static DEFINE_MUTEX(registration_lock);
static unsigned int platform_users;
static EMI_CTRL_STATE_OFFSET emi_offsets = {
	.emi_apmem_ctrl_state = 0, .emi_apmem_ctrl_host_sync_state = 4,
	.emi_apmem_ctrl_host_sync_num = 8, .emi_apmem_ctrl_chip_sync_state = 12,
	.emi_apmem_ctrl_chip_sync_num = 16, .emi_apmem_ctrl_chip_sync_addr = 20,
	.emi_apmem_ctrl_chip_sync_len = 24, .emi_apmem_ctrl_chip_print_buff_start = 28,
	.emi_apmem_ctrl_chip_print_buff_len = 32, .emi_apmem_ctrl_chip_print_buff_idx = 36,
	.emi_apmem_ctrl_chip_int_status = 40, .emi_apmem_ctrl_chip_paded_dump_end = 44,
	.emi_apmem_ctrl_host_outband_assert_w1 = 48,
};
static CONSYS_EMI_ADDR_INFO emi_info = {
	.emi_phy_addr = 0xf0080000, .paged_trace_off = 0x400,
	.paged_dump_off = 0x8400, .full_dump_off = 0x10400, .p_ecso = &emi_offsets,
};

static int switch_ldo(struct regulator *regulator, bool *state, bool enable)
{
	int error;
	if (*state == enable)
		return 0;
	error = enable ? regulator_enable(regulator) : regulator_disable(regulator);
	if (!error)
		*state = enable;
	return error;
}

/* Exact independent stock MPU windows: region5 belongs to Wi-Fi firmware
 * (CONN only), region6 to the shared WMT/debug area (AP and CONN). Their
 * setup is independent of the optional APDMA transport. All this memory is
 * reserved/no-map; never map or clear Wi-Fi's AP-forbidden private window. */
static int configure_emi_mpu(struct native_consys *c)
{
 struct mpu_window {
  u32 boundary, permissions, shift, access, bounds;
  u32 old_bounds, old_permissions;
  bool touched;
 } windows[] = {
  { .boundary = EMI_REGION5, .permissions = EMI_PERMISSIONS45,
    .shift = 16, .access = EMI_WIFI_PERMISSION },
  { .boundary = EMI_REGION6, .permissions = EMI_PERMISSIONS67,
    .shift = 0, .access = EMI_CONN_PERMISSION },
 };
 phys_addr_t first, last;
 unsigned long flags;
 unsigned int i;
 int error = 0;
 if (c->emi_base < 0x80000000ULL || c->emi_base + SZ_1M - 1 > 0xffffffffULL)
  return -ERANGE;
 for (i = 0; i < ARRAY_SIZE(windows); i++) {
  first = c->emi_base + i * SZ_512K;
  last = first + SZ_512K - 1;
  windows[i].bounds = ((u32)((first - 0x80000000ULL) >> 16) << 16) |
                     (u32)((last - 0x80000000ULL) >> 16);
 }
 spin_lock_irqsave(&c->mpu_lock, flags);
 /* Snapshot both own windows before making any changes. Unknown attribute
  * bits may have hardware semantics absent from this BSP: reject them. */
 for (i = 0; i < ARRAY_SIZE(windows); i++) {
  struct mpu_window *w = &windows[i];
  w->old_bounds = readl(c->mpu + w->boundary);
  w->old_permissions = readl(c->mpu + w->permissions);
  if ((w->old_permissions >> w->shift) & 0xf000) {
   error = -EACCES;
   goto unlock;
  }
 }
 for (i = 0; i < ARRAY_SIZE(windows); i++) {
  struct mpu_window *w = &windows[i];
  u32 mask = 0xffffU << w->shift;
  u32 preserve = w->old_permissions & ~mask;
  u32 expected = preserve | (w->access << w->shift);
  if (w->old_bounds != w->bounds || w->old_permissions != expected) {
   w->touched = true;
   /* Temporarily disable only the own half; preserve region4/7 at every
    * step instead of the old BSP's full-word permission zero. */
   writel(preserve, c->mpu + w->permissions);
   readl(c->mpu + w->permissions);
   writel(w->bounds, c->mpu + w->boundary);
   readl(c->mpu + w->boundary);
   writel(expected, c->mpu + w->permissions);
  }
  if (readl(c->mpu + w->boundary) != w->bounds ||
      readl(c->mpu + w->permissions) != expected) {
   error = -EIO;
   goto rollback;
  }
 }
 goto unlock;
rollback:
 for (i = 0; i < ARRAY_SIZE(windows); i++) {
  struct mpu_window *w = &windows[i];
  u32 mask = 0xffffU << w->shift;
  u32 preserve;
  if (!w->touched) continue;
  preserve = readl(c->mpu + w->permissions) & ~mask;
  writel(preserve, c->mpu + w->permissions);
  readl(c->mpu + w->permissions);
  writel(w->old_bounds, c->mpu + w->boundary);
  readl(c->mpu + w->boundary);
  writel(preserve | (w->old_permissions & mask), c->mpu + w->permissions);
  readl(c->mpu + w->permissions);
 }
unlock:
 spin_unlock_irqrestore(&c->mpu_lock, flags);
 if (error) {
  dev_err(c->dev, "EMI MPU setup failed (%d): r5=%08x p45=%08x r6=%08x p67=%08x\n",
          error, readl(c->mpu + EMI_REGION5), readl(c->mpu + EMI_PERMISSIONS45),
          readl(c->mpu + EMI_REGION6), readl(c->mpu + EMI_PERMISSIONS67));
  return error;
 }
 dev_info(c->dev, "EMI MPU r5=%08x p45=%08x r6=%08x p67=%08x; violation=%08x/%08x retained\n",
          readl(c->mpu + EMI_REGION5), readl(c->mpu + EMI_PERMISSIONS45),
          readl(c->mpu + EMI_REGION6), readl(c->mpu + EMI_PERMISSIONS67),
          readl(c->mpu + EMI_VIOLATION), readl(c->mpu + EMI_VIOLATION_ADDRESS));
 return 0;
}

static int consys_on(struct native_consys *c)
{
	u32 value;
	int error;
	if (c->on)
		return 0;
	error = mt6580_connsys_mcu_reset(true);
	if (error)
		return error;
	error = regulator_enable(c->v18);
	if (error)
		return error;
	udelay(300);
	/* The board declares CLKBUF2 HW ownership: do not replace its group
	 * with another board's software CONNSRC sequence. Only choose the RF
	 * XO source in the GPIO block, exactly as the stock co_clock_flag=1. */
	value = readl(c->gpio + 0x13c);
	writel((value & ~GENMASK(7, 6)) | BIT(7), c->gpio + 0x13c);
	value = readl(c->top + 0x1320);
	writel((value & ~GENMASK(12, 0)) | (c->emi_base >> 20) | BIT(12), c->top + 0x1320);
	error = pm_runtime_resume_and_get(c->dev);
	if (error < 0)
		goto disable_18;
	udelay(10);
	error = readl_poll_timeout(c->mcu + 8, value, value == 0x6580, 20000, 200000);
	if (error)
		goto power_off;
	c->chipid = value;
	writel(readl(c->mcu + 0x110) | BIT(18), c->mcu + 0x110);
	error = mt6580_connsys_mcu_reset(false);
	if (error)
		goto power_off;
	msleep(20); /* Boot ROM/oscillator settle; STP patch commands follow. */
	c->on = true; c->power_ons++;
	dev_info(c->dev, "CONSYS MCU started: chip=%x EMI=%pa map=%x\n",
		 c->chipid, &c->emi_base, readl(c->top + 0x1320));
	return 0;
power_off:
	mt6580_connsys_mcu_reset(true);
	pm_runtime_put_sync(c->dev);
disable_18:
	regulator_disable(c->v18);
	return error;
}

static int consys_off(struct native_consys *c)
{
	int error, result;
	if (!c->on)
		return 0;
	/* SCPSYS protects/drains AXI first, then asserts the shared MCU reset. */
	if (atomic_xchg(&c->bgf_enabled, 0))
		disable_irq(c->bgf_irq);
	error = pm_runtime_put_sync_suspend(c->dev);
	if (error < 0) {
        pm_runtime_get_noresume(c->dev);
        return error;
    }
	error = 0;
	result = switch_ldo(c->v33_wifi, &c->wifi_ldo, false);
	if (result && !error) error = result;
	result = switch_ldo(c->v33_bt, &c->bt_ldo, false);
	if (result && !error) error = result;
	result = switch_ldo(c->v28, &c->v28_on, false);
	if (result && !error) error = result;
	result = regulator_disable(c->v18);
	if (result && !error) error = result;
	if (!error) { c->on = false; c->power_offs++; }
	return error;
}

INT32 wmt_plat_pwr_ctrl(ENUM_FUNC_STATE state)
{
	struct native_consys *c = con;
	int error;
	if (!c)
		return -ENODEV;
	mutex_lock(&c->lock);
	if (state == FUNC_STAT) error = c->on;
	else if (state == FUNC_ON) error = consys_on(c);
	else if (state == FUNC_OFF) error = consys_off(c);
	else if (state == FUNC_RST) { error = consys_off(c); if (!error) error = consys_on(c); }
	else error = -EINVAL;
	c->last_error = error;
	mutex_unlock(&c->lock);
	return error;
}

INT32 wmt_plat_soc_paldo_ctrl(ENUM_PALDO_TYPE type, ENUM_PALDO_OP operation)
{
	struct native_consys *c = con;
	int result;
	bool enable = operation == PALDO_ON;
	if (!c)
		return -ENODEV;
	mutex_lock(&c->lock);
	if (type == PMIC_CHIPID_PALDO) {
		struct regmap *map = regulator_get_regmap(c->v18);
		unsigned int cid;
		result = IS_ERR_OR_NULL(map) ? (map ? PTR_ERR(map) : -ENODEV) : regmap_read(map, MT6350_CID, &cid);
		if (!result) result = cid;
	} else if (operation != PALDO_ON && operation != PALDO_OFF) result = -EINVAL;
	else if (type == WIFI_PALDO) result = switch_ldo(c->v33_wifi, &c->wifi_ldo, enable);
	else if (type == BT_PALDO) result = switch_ldo(c->v33_bt, &c->bt_ldo, enable);
	else if (type == GPS_PALDO || type == FM_PALDO) result = switch_ldo(c->v28, &c->v28_on, enable);
    else if (type == EFUSE_PALDO) {
        /* This board's native path implements only its verified co-clock=1
         * configuration; the stock EFUSE window supplies VCN28 in that mode. */
        if ((wmt_lib_co_clock_get() & 0xf) != 1) result = -EOPNOTSUPP;
        else result = switch_ldo(c->v28, &c->v28_on, enable);
    }
	else result = -EOPNOTSUPP;
	mutex_unlock(&c->lock);
	return result;
}

/* Mapping follows the owner's wmt_set_pmic_voltage jump table/literals,
 * not a guessed nominal rail. Both consumers share MT6350 VCN33 VOSEL. */
int mt6580_consys_set_vcn33(unsigned int level)
{
 static const int microvolts[] = {3300000, 3400000, 3500000, 3600000};
 struct native_consys *c = con;
 int old_bt, old_wifi, target, error, rollback_bt, rollback_wifi;
 if (!c) return -ENODEV;
 if (level >= ARRAY_SIZE(microvolts)) return -EINVAL;
 target = microvolts[level];
 mutex_lock(&c->lock);
 old_bt = regulator_get_voltage(c->v33_bt);
 old_wifi = regulator_get_voltage(c->v33_wifi);
 if (old_bt <= 0 || old_wifi <= 0) {
  error = old_bt < 0 ? old_bt : old_wifi < 0 ? old_wifi : -EINVAL;
  goto out;
 }
 error = regulator_set_voltage(c->v33_bt, target, target);
 if (!error) error = regulator_set_voltage(c->v33_wifi, target, target);
 if (!error && (regulator_get_voltage(c->v33_bt) != target ||
                regulator_get_voltage(c->v33_wifi) != target)) error = -EIO;
 if (error) {
  rollback_bt = regulator_set_voltage(c->v33_bt, old_bt, old_bt);
  rollback_wifi = regulator_set_voltage(c->v33_wifi, old_wifi, old_wifi);
  if (rollback_bt || rollback_wifi)
   dev_err(c->dev, "VCN33 rollback failed: BT=%d WiFi=%d after error=%d\n",
           rollback_bt, rollback_wifi, error);
  goto out;
 }
 c->vcn33_uv = target;
 c->efuse_level = level;
 dev_info(c->dev, "Actual EFUSE level=%u selected BT/WiFi VCN33=%duV\n", level, target);
out:
 c->last_error = error;
 mutex_unlock(&c->lock);
 return error;
}

INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 on)
{
	return wmt_plat_soc_paldo_ctrl(WIFI_PALDO, on ? PALDO_ON : PALDO_OFF);
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_wifi_paldo_ctrl);

static irqreturn_t bgf_interrupt(int irq, void *data)
{
	struct native_consys *c = data;
	irq_cb callback = READ_ONCE(c->bgf_callback);
	if (!READ_ONCE(c->on))
		return IRQ_NONE;
	if (atomic_xchg(&c->bgf_enabled, 0))
		disable_irq_nosync(irq);
	pm_wakeup_event(c->dev, 500);
	if (callback)
		callback();
	return IRQ_HANDLED;
}

VOID wmt_plat_irq_cb_reg(irq_cb callback) {
 WRITE_ONCE(registered_bgf_callback, callback);
 if (con) WRITE_ONCE(con->bgf_callback, callback);
}
INT32 wmt_plat_eirq_ctrl(ENUM_PIN_ID pin, ENUM_PIN_STATE state)
{
	struct native_consys *c = con;
	if (!c || pin != PIN_BGF_EINT)
		return -ENODEV;
	if (state == PIN_STA_INIT) return 0; /* devm_request_irq has completed. */
	if (state == PIN_STA_EINT_EN) {
		if (!atomic_xchg(&c->bgf_enabled, 1)) enable_irq(c->bgf_irq);
	} else if (state == PIN_STA_EINT_DIS || state == PIN_STA_DEINIT) {
		if (atomic_xchg(&c->bgf_enabled, 0)) disable_irq(c->bgf_irq);
	} else return -EINVAL;
	return 0;
}
INT32 wmt_plat_wake_lock_ctrl(ENUM_WL_OP operation)
{
	if (!con) return -ENODEV;
	if (operation == WL_OP_GET) __pm_stay_awake(con->work_lock);
	else if (operation == WL_OP_PUT) __pm_relax(con->work_lock);
	else return -EINVAL;
	return 0;
}
UINT8 *wmt_plat_get_emi_virt_add(UINT32 offset)
{
	if (!con || offset > DUMP_SIZE - 4) return NULL;
	return (UINT8 __force *)(con->emi + offset);
}
P_CONSYS_EMI_ADDR_INFO wmt_plat_get_emi_phy_add(void) { return con ? &emi_info : NULL; }
UINT32 wmt_plat_get_soc_chipid(void) { return 0x6580; } /* Platform type, not a fabricated MCU read. */
UINT32 wmt_plat_read_cpupcr(void) { return con && con->on ? readl(con->mcu + 0x160) : 0; }
UINT32 wmt_plat_read_dmaregs(UINT32 index) { return 0; } /* No APDMA is used by this PIO transport. */
INT32 wmt_plat_gpio_ctrl(ENUM_PIN_ID pin, ENUM_PIN_STATE state) { return -EOPNOTSUPP; }
INT32 wmt_plat_audio_ctrl(CMB_STUB_AIF_X state, CMB_STUB_AIF_CTRL control) { return -EOPNOTSUPP; }
VOID wmt_plat_aif_cb_reg(device_audio_if_cb callback) { WRITE_ONCE(audio_callback, callback); }
VOID wmt_plat_func_ctrl_cb_reg(func_ctrl_cb callback) { WRITE_ONCE(function_callback, callback); }
VOID wmt_plat_thermal_ctrl_cb_reg(thermal_query_ctrl_cb callback) { WRITE_ONCE(thermal_callback, callback); }
VOID wmt_plat_deep_idle_ctrl_cb_reg(deep_idle_ctrl_cb callback) { WRITE_ONCE(deep_callback, callback); }
/* WMT PSM enters after the firmware sleep acknowledgement and restores the
 * AP BTIF clock before requesting a real firmware wake acknowledgement. */
int mt_combo_plt_enter_deep_idle(COMBO_IF source)
{
 deep_idle_ctrl_cb callback = READ_ONCE(deep_callback);
 if (source != COMBO_IF_BTIF || !callback) return -EOPNOTSUPP;
 return callback(BTIF_DPIDLE_ENABLE);
}
int mt_combo_plt_exit_deep_idle(COMBO_IF source)
{
 deep_idle_ctrl_cb callback = READ_ONCE(deep_callback);
 if (source != COMBO_IF_BTIF || !callback) return -EOPNOTSUPP;
 return callback(BTIF_DPIDLE_DISABLE);
}
int mtk_wcn_wmt_chipid_query(void)
{
 /* Legacy query is the selected platform, including before its MCU boots. */
 return con ? 0x6580 : -ENODEV;
}
UINT32 wmt_plat_jtag_flag_ctrl(UINT32 enable) { return -EOPNOTSUPP; }
VOID wmt_plat_BGF_irq_dump_status(void) { if (con) dev_info(con->dev, "BGF irq=%d enabled=%u\n", con->bgf_irq, atomic_read(&con->bgf_enabled)); }
MTK_WCN_BOOL wmt_plat_dump_BGF_irq_status(void) { return con && atomic_read(&con->bgf_enabled); }
INT32 wmt_plat_set_host_dump_state(ENUM_HOST_DUMP_STATE state)
{
	if (!con) return -ENODEV;
	writel(state, con->emi + 4); return state;
}
INT32 wmt_plat_update_host_sync_num(void)
{
	u32 value;
	if (!con) return -ENODEV;
	value = readl(con->emi + 8) + 1; writel(value, con->emi + 8); return value;
}
INT32 wmt_plat_get_dump_info(UINT32 offset)
{
	if (!con || (offset & 3) || offset > DUMP_SIZE - 4) return -EINVAL;
	return readl(con->emi + offset);
}
UINT32 wmt_plat_force_trigger_assert(ENUM_FORCE_TRG_ASSERT_T type)
{
	if (!con || !con->on) return -ENODEV;
	if (type != STP_FORCE_TRG_ASSERT_EMI) return -EOPNOTSUPP;
	writel(0x19b30bb1, con->emi + 0x30); return 0;
}
INT32 wmt_plat_get_tdm_antsel_index(void) { return -EOPNOTSUPP; }

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buffer)
{
	struct native_consys *c = dev_get_drvdata(dev);
	return sysfs_emit(buffer, "on=%u chipid=%04x power_ons=%llu power_offs=%llu error=%d wifi_ldo=%u bt_ldo=%u bgf_enabled=%u vcn28=%u efuse_level=%u vcn33_uv=%d mpu5=%08x mpu45=%08x mpu6=%08x mpu67=%08x violation=%08x/%08x\n",
		c->on, c->chipid, c->power_ons, c->power_offs, c->last_error, c->wifi_ldo, c->bt_ldo, atomic_read(&c->bgf_enabled), c->v28_on, c->efuse_level, c->vcn33_uv,
        readl(c->mpu + EMI_REGION5), readl(c->mpu + EMI_PERMISSIONS45),
        readl(c->mpu + EMI_REGION6), readl(c->mpu + EMI_PERMISSIONS67),
        readl(c->mpu + EMI_VIOLATION), readl(c->mpu + EMI_VIOLATION_ADDRESS));
}
static DEVICE_ATTR_RO(state);

static void __iomem *map_shared(struct platform_device *pdev, const char *name)
{
	struct resource *resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource) return ERR_PTR(-EINVAL);
	return devm_ioremap(&pdev->dev, resource->start, resource_size(resource)) ?: ERR_PTR(-ENOMEM);
}

static int consys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct native_consys *c;
	struct device_node *memory;
	struct reserved_mem *reserved;
	struct clk *btif;
	void __iomem *base;
	int error, btif_irq;
	if (con) return -EBUSY;
	if (!dev->pm_domain) return -EPROBE_DEFER;
    /* genpd may inherit an already-on boot domain and skip power_on. Stop
     * any retained MCU before taking ownership of/clearing its shared EMI. */
    error = mt6580_connsys_mcu_reset(true);
    if (error) return dev_err_probe(dev, error, "hold CONSYS MCU reset\n");
	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c) return -ENOMEM;
	c->dev = dev; mutex_init(&c->lock); spin_lock_init(&c->mpu_lock);
 c->bgf_callback = READ_ONCE(registered_bgf_callback);
	c->mcu = map_shared(pdev, "mcu"); c->top = map_shared(pdev, "top");
	c->gpio = map_shared(pdev, "gpio"); c->spm = map_shared(pdev, "spm");
    c->mpu = map_shared(pdev, "emi-mpu");
	base = map_shared(pdev, "btif");
	if (IS_ERR(c->mcu) || IS_ERR(c->top) || IS_ERR(c->gpio) || IS_ERR(c->spm) || IS_ERR(c->mpu) || IS_ERR(base)) return -EINVAL;
	if (!of_property_read_bool(dev->of_node, "mediatek,clkbuf-hardware-controlled") ||
	    (readl(c->spm + 0x620) & GENMASK(9, 8)))
		return dev_err_probe(dev, -EINVAL, "CONNSRC clock buffer ownership is not the qualified board HW mode\n");
	c->v18 = devm_regulator_get(dev, "vcn18"); c->v28 = devm_regulator_get(dev, "vcn28");
	c->v33_wifi = devm_regulator_get(dev, "vcn33-wifi"); c->v33_bt = devm_regulator_get(dev, "vcn33-bt");
	if (IS_ERR(c->v18)) return dev_err_probe(dev, PTR_ERR(c->v18), "vcn18\n");
	if (IS_ERR(c->v28)) return dev_err_probe(dev, PTR_ERR(c->v28), "vcn28\n");
	if (IS_ERR(c->v33_wifi)) return dev_err_probe(dev, PTR_ERR(c->v33_wifi), "vcn33-wifi\n");
	if (IS_ERR(c->v33_bt)) return dev_err_probe(dev, PTR_ERR(c->v33_bt), "vcn33-bt\n");
	memory = of_parse_phandle(dev->of_node, "memory-region", 0);
	reserved = memory ? of_reserved_mem_lookup(memory) : NULL;
	of_node_put(memory);
	if (!reserved || reserved->size < DUMP_OFFSET + DUMP_SIZE || (reserved->base & (SZ_1M - 1)))
		return -EINVAL;
	c->emi_base = reserved->base;
    error = configure_emi_mpu(c);
    if (error) return error;
	c->emi = devm_ioremap(dev, reserved->base + DUMP_OFFSET, DUMP_SIZE);
	if (!c->emi) return -ENOMEM;
	memset_io(c->emi, 0, DUMP_SIZE);
	btif = devm_clk_get(dev, "btif");
	if (IS_ERR(btif)) return dev_err_probe(dev, PTR_ERR(btif), "BTIF clock\n");
	c->bgf_irq = platform_get_irq_byname(pdev, "bgf");
	btif_irq = platform_get_irq_byname(pdev, "btif");
	if (c->bgf_irq < 0 || btif_irq < 0) return -EINVAL;
	error = devm_request_irq(dev, c->bgf_irq, bgf_interrupt, IRQF_NO_AUTOEN, "mt6580-bgf", c);
	if (error) return error;
	error = mt6580_btif_init(dev, base, btif, btif_irq);
	if (error) return error;
	c->work_lock = wakeup_source_register(dev, "mt6580-wmt");
	if (!c->work_lock) { mt6580_btif_exit(); return -ENOMEM; }
	platform_set_drvdata(pdev, c); con = c;
	device_init_wakeup(dev, true);
	pm_runtime_set_active(dev); pm_runtime_enable(dev); pm_runtime_idle(dev);
	error = device_create_file(dev, &dev_attr_state);
	if (error) dev_warn(dev, "state attribute failed: %d\n", error);
	dev_info(dev, "CONSYS resources registered; firmware remains stopped\n");
	return 0;
}
static void consys_remove(struct platform_device *pdev)
{
	struct native_consys *c = platform_get_drvdata(pdev);
	mt6580_btif_exit();
	mutex_lock(&c->lock);
	consys_off(c);
	mutex_unlock(&c->lock);
	pm_runtime_disable(c->dev);
	device_remove_file(c->dev, &dev_attr_state);
	device_init_wakeup(c->dev, false);
	wakeup_source_unregister(c->work_lock);
	con = NULL;
}
/* Keep the running connectivity domain and BTIF state intact. Parent SPM
 * refuses DDR/infra/PLL removal while CONN is powered; BGF is the real WMT
 * firmware wake interrupt. Full WLAN firmware ownership is qualified later. */
static int consys_suspend(struct device *dev)
{
 struct native_consys *c = dev_get_drvdata(dev);
 int error = 0;
 mutex_lock(&c->lock);
 if (c->on && device_may_wakeup(dev)) {
  error = enable_irq_wake(c->bgf_irq);
  if (!error) c->bgf_wake = true;
 }
 mutex_unlock(&c->lock);
 return error;
}
static int consys_resume(struct device *dev)
{
 struct native_consys *c = dev_get_drvdata(dev);
 int error = 0;
 mutex_lock(&c->lock);
 if (c->bgf_wake) {
  error = disable_irq_wake(c->bgf_irq);
  if (!error) c->bgf_wake = false;
 }
 mutex_unlock(&c->lock);
 return error;
}
static DEFINE_SIMPLE_DEV_PM_OPS(consys_pm_ops, consys_suspend, consys_resume);
static const struct of_device_id consys_match[] = { { .compatible = "mediatek,mt6580-consys" }, {} };
static struct platform_driver consys_driver = {
	.probe = consys_probe,
	.remove = consys_remove,
	.driver = { .name = "mt6580-consys", .of_match_table = consys_match, .suppress_bind_attrs = true, .pm = pm_sleep_ptr(&consys_pm_ops) },
};
INT32 wmt_plat_init(UINT32 co_clock)
{
	int result = 0;
	mutex_lock(&registration_lock);
	if (!platform_users) result = platform_driver_register(&consys_driver);
	if (!result) platform_users++;
	mutex_unlock(&registration_lock);
	return result;
}
INT32 wmt_plat_deinit(void)
{
	mutex_lock(&registration_lock);
	if (platform_users && !--platform_users) platform_driver_unregister(&consys_driver);
	mutex_unlock(&registration_lock);
	return 0;
}
