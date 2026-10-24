/* SPDX-License-Identifier: GPL-2.0-only */
/* Stateful OCV/resistance observer based on the MT6580 BSP oam_i_2/car_2
 * algorithm. No measured current or hardware coulomb counter is available.
 */
#ifndef _MT6350_BATTERY_MODEL_H
#define _MT6350_BATTERY_MODEL_H

#include "mt6350-battery-profile.h"

struct mt6350_battery_model {
	s64 charge_uas;
	s64 full_uas;
	s64 remainder;
	int ocv_uv;
	int resistance_mohm;
	int current_ua; /* Estimated, positive means discharge. */
};

static int mt6350_lerp(int x, int x0, int x1, int y0, int y1)
{
	if (x0 == x1)
		return y0;
	return y0 + div_s64((s64)(x - x0) * (y1 - y0), x1 - x0);
}

static int mt6350_profile_resistance(const struct mt6350_resistance_point *p,
				     int n, int uv)
{
	int i, mv = uv / 1000;

	if (mv >= p[0].mv)
		return p[0].mohms;
	for (i = 1; i < n; i++)
		if (mv >= p[i].mv)
			return mt6350_lerp(mv, p[i].mv, p[i - 1].mv,
					   p[i].mohms, p[i - 1].mohms);
	return p[n - 1].mohms;
}

static int mt6350_model_resistance(int uv, int temp)
{
	int r0 = mt6350_profile_resistance(mt6350_r1, ARRAY_SIZE(mt6350_r1), uv);
	int r25 = mt6350_profile_resistance(mt6350_r2, ARRAY_SIZE(mt6350_r2), uv);
	int r50 = mt6350_profile_resistance(mt6350_r3, ARRAY_SIZE(mt6350_r3), uv);

	if (temp <= 0)
		return r0;
	if (temp < 250)
		return mt6350_lerp(temp, 0, 250, r0, r25);
	if (temp < 500)
		return mt6350_lerp(temp, 250, 500, r25, r50);
	return r50;
}

/* Use fractional percent (ppm of full capacity), not integer percent, so
 * the observer does not stick at a single OCV until a whole percent changes.
 */
static int mt6350_table_ocv(const struct power_supply_battery_ocv_table *t,
			   int n, int ppm)
{
	int i;

	if (ppm >= t[0].capacity * 10000)
		return t[0].ocv;
	for (i = 1; i < n; i++)
		if (ppm >= t[i].capacity * 10000)
			return mt6350_lerp(ppm, t[i].capacity * 10000,
					   t[i - 1].capacity * 10000,
					   t[i].ocv, t[i - 1].ocv);
	return t[n - 1].ocv;
}

static int mt6350_model_ocv(struct power_supply_battery_info *info,
			   int ppm, int temp)
{
	int i, a, b;

	/* Find bracketing temperatures without depending on DT table order. */
	a = b = -1;
	for (i = 0; i < POWER_SUPPLY_OCV_TEMP_MAX; i++) {
		if (!info->ocv_table[i])
			continue;
		if (info->ocv_temp[i] * 10 <= temp &&
		    (a < 0 || info->ocv_temp[i] > info->ocv_temp[a]))
			a = i;
		if (info->ocv_temp[i] * 10 >= temp &&
		    (b < 0 || info->ocv_temp[i] < info->ocv_temp[b]))
			b = i;
	}
	if (a < 0)
		a = b;
	if (b < 0)
		b = a;
	return mt6350_lerp(temp, info->ocv_temp[a] * 10, info->ocv_temp[b] * 10,
		mt6350_table_ocv(info->ocv_table[a], info->ocv_table_size[a], ppm),
		mt6350_table_ocv(info->ocv_table[b], info->ocv_table_size[b], ppm));
}

static void mt6350_model_init(struct mt6350_battery_model *m,
			      struct power_supply_battery_info *info,
			      int uv, int temp)
{
	int lo = 0, hi = 1000000, mid;

	/* Invert the same temperature-interpolated OCV curve used at runtime. */
	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (mt6350_model_ocv(info, mid, temp) < uv)
			lo = mid + 1;
		else
			hi = mid;
	}
	m->full_uas = (s64)info->charge_full_design_uah * 3600;
	m->charge_uas = div_s64(m->full_uas * lo, 1000000);
	m->remainder = 0;
}

static int mt6350_model_ppm(const struct mt6350_battery_model *m)
{
	return div64_s64(m->charge_uas * 1000000, m->full_uas);
}

static void mt6350_model_step(struct mt6350_battery_model *m,
			      struct power_supply_battery_info *info,
			      int uv, int temp, int dt_ms, bool online,
			      int charge_limit_ua)
{
	s64 delta, integral;
	int estimated_ua;

	m->ocv_uv = mt6350_model_ocv(info, mt6350_model_ppm(m), temp);
	m->resistance_mohm = mt6350_model_resistance(m->ocv_uv, temp);
	estimated_ua = div_s64((s64)(m->ocv_uv - uv) * 1000, m->resistance_mohm);
	/* No charge is possible without an external source. While connected,
	 * allow net discharge: a USB supply may not cover the system load.
	 * Bound the observer, not the charger, to 2C discharge and configured
	 * charge current. The latter is a limit, never a measured current.
	 */
	m->current_ua = clamp(estimated_ua, online ? -charge_limit_ua : 0,
			      info->charge_full_design_uah * 2);
	delta = (s64)m->current_ua * dt_ms + m->remainder;
	integral = div_s64(delta, 1000);
	m->charge_uas -= integral;
	m->remainder = delta - integral * 1000;
	m->charge_uas = clamp_t(s64, m->charge_uas, 0, m->full_uas);
}

#endif
