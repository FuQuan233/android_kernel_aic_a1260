/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6350 software fuel gauge reconstructed from the A1260 factory kernel.
 * The OAM observers estimate current from voltage and resistance; neither
 * observer is a measured-current input or a hardware coulomb counter.
 */
#ifndef _MT6350_BATTERY_MODEL_H
#define _MT6350_BATTERY_MODEL_H

#include <linux/array_size.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/string.h>
#include <linux/types.h>

#include "mt6350-battery-profile.h"

#define MT6350_MODEL_TEMP_WINDOW	30
#define MT6350_MODEL_R_FG_MOHM	10

struct mt6350_battery_model {
	int ocv1_mv;
	int ocv2_mv;
	int r1_mohm;
	int r2_mohm;
	int current1_01ma; /* Estimated, positive means discharge. */
	int current2_01ma;
	s64 car1_01mah;
	s64 car2_01mah;
	int dod0;
	int dod1;
	int dod2;
	int dod3;
	int qmax_mah;
	struct mt6350_profile_point profile[MT6350_PROFILE_SIZE];
	int profile_temp_c;
	int temp_history[MT6350_MODEL_TEMP_WINDOW];
	int temp_sum_c;
	int temp_avg_c;
	unsigned int temp_index;
	bool temp_history_ready;
};

static inline int mt6350_model_temp_bracket(int *temp_c)
{
	int upper;

	*temp_c = clamp(*temp_c, mt6350_profile_temps[0],
			mt6350_profile_temps[ARRAY_SIZE(mt6350_profile_temps) - 1]);
	for (upper = 1; upper < ARRAY_SIZE(mt6350_profile_temps) - 1; upper++)
		if (*temp_c <= mt6350_profile_temps[upper])
			break;

	return upper;
}

/* The factory profile constructor interpolates upwards from the smaller
 * ordinate. Keep that rounding, including decreasing temperature curves.
 */
static inline int mt6350_model_profile_lerp(int temp, int lo, int hi,
					  int low_value, int high_value)
{
	if (high_value > low_value)
		return low_value + div_s64((s64)(temp - lo) *
					  (high_value - low_value), hi - lo);

	return high_value + div_s64((s64)(hi - temp) *
				   (low_value - high_value), hi - lo);
}

static inline void mt6350_model_build_profile(struct mt6350_battery_model *m,
					      int temp_c)
{
	const struct mt6350_profile_point *lo, *hi;
	int upper = mt6350_model_temp_bracket(&temp_c);
	int low_temp = mt6350_profile_temps[upper - 1];
	int high_temp = mt6350_profile_temps[upper];
	int i;

	lo = mt6350_factory_profiles[upper - 1];
	hi = mt6350_factory_profiles[upper];
	for (i = 0; i < MT6350_PROFILE_SIZE; i++) {
		m->profile[i].dod = hi[i].dod;
		m->profile[i].mv = mt6350_model_profile_lerp(temp_c,
			low_temp, high_temp, lo[i].mv, hi[i].mv);
		m->profile[i].mohms = mt6350_model_profile_lerp(temp_c,
			low_temp, high_temp, lo[i].mohms, hi[i].mohms);
	}
	m->profile_temp_c = temp_c;
}

static inline int mt6350_model_qmax(int temp_c)
{
	int upper = mt6350_model_temp_bracket(&temp_c);
	int low_temp = mt6350_profile_temps[upper - 1];
	int high_temp = mt6350_profile_temps[upper];
	int low_qmax = mt6350_factory_qmax[upper - 1];
	int high_qmax = mt6350_factory_qmax[upper];

	return low_qmax + div_s64((s64)(temp_c - low_temp) *
				 (high_qmax - low_qmax), high_temp - low_temp);
}

static inline void mt6350_model_update_profile(struct mt6350_battery_model *m,
					       int temp_c)
{
	int avg, i;

	if (!m->temp_history_ready) {
		for (i = 0; i < MT6350_MODEL_TEMP_WINDOW; i++)
			m->temp_history[i] = temp_c;
		m->temp_sum_c = temp_c * MT6350_MODEL_TEMP_WINDOW;
		m->temp_avg_c = temp_c;
		m->temp_history_ready = true;
	}

	m->temp_sum_c -= m->temp_history[m->temp_index];
	m->temp_history[m->temp_index] = temp_c;
	m->temp_sum_c += temp_c;
	avg = m->temp_sum_c / MT6350_MODEL_TEMP_WINDOW;
	/* As in fgauge_construct_table_by_temp: a change in the 30-sample
	 * mean triggers reconstruction using the current temperature.
	 */
	if (avg != m->temp_avg_c) {
		mt6350_model_build_profile(m, temp_c);
		m->temp_avg_c = avg;
	}
	m->temp_index = (m->temp_index + 1) % MT6350_MODEL_TEMP_WINDOW;
}

static inline int mt6350_model_dod_by_voltage(const struct mt6350_battery_model *m,
					      int mv)
{
	const struct mt6350_profile_point *p = m->profile;
	int i;

	if (mv > p[0].mv)
		return 0;
	if (mv < p[MT6350_PROFILE_SIZE - 1].mv)
		return 100;
	for (i = 0; i < MT6350_PROFILE_SIZE - 1; i++) {
		if (mv > p[i].mv || mv < p[i + 1].mv)
			continue;
		if (p[i].mv == p[i + 1].mv)
			return p[i].dod;
		return p[i].dod + div_s64((s64)(p[i].mv - mv) *
			(p[i + 1].dod - p[i].dod), p[i].mv - p[i + 1].mv);
	}

	return 0;
}

static inline int mt6350_model_soc_by_voltage(const struct mt6350_battery_model *m,
					      int mv)
{
	return 100 - clamp(mt6350_model_dod_by_voltage(m, mv), 0, 100);
}

static inline int mt6350_model_voltage_by_soc(const struct mt6350_battery_model *m,
					      int soc)
{
	const struct mt6350_profile_point *p = m->profile;
	int i, dod = 100 - soc;

	if (dod < p[0].dod || dod > p[MT6350_PROFILE_SIZE - 1].dod)
		return 3700;
	for (i = 0; i < MT6350_PROFILE_SIZE - 1; i++) {
		if (dod < p[i].dod || dod > p[i + 1].dod)
			continue;
		if (p[i].dod == p[i + 1].dod)
			return p[i].mv;
		return p[i].mv - div_s64((s64)(dod - p[i].dod) *
			(p[i].mv - p[i + 1].mv), p[i + 1].dod - p[i].dod);
	}

	return 0;
}

static inline int mt6350_model_resistance(const struct mt6350_battery_model *m,
					int mv)
{
	const struct mt6350_profile_point *p = m->profile;
	int i;

	if (mv > p[0].mv)
		return p[0].mohms;
	if (mv < p[MT6350_PROFILE_SIZE - 1].mv)
		return p[MT6350_PROFILE_SIZE - 1].mohms;
	for (i = 0; i < MT6350_PROFILE_SIZE - 1; i++) {
		if (mv > p[i].mv || mv < p[i + 1].mv)
			continue;
		if (p[i].mv == p[i + 1].mv)
			return p[i].mohms;
		return p[i].mohms + div_s64((s64)(p[i].mv - mv) *
			(p[i + 1].mohms - p[i].mohms), p[i].mv - p[i + 1].mv);
	}

	return 0;
}

static inline int mt6350_model_imp_tracking(const struct mt6350_battery_model *m,
					  int mv, int current_01ma)
{
	int corrected_mv = mv;
	int i, resistance;
	s64 compensate;

	for (i = 0; i < 5; i++) {
		resistance = mt6350_model_resistance(m, corrected_mv);
		compensate = div_s64((s64)current_01ma *
				    (resistance + MT6350_MODEL_R_FG_MOHM), 1000);
		compensate = div_s64(compensate + 5, 10);
		corrected_mv = mv + compensate;
	}
	/* The factory FG_METER_RESISTANCE contribution is zero. */
	resistance = mt6350_model_resistance(m, corrected_mv);
	compensate = div_s64((s64)current_01ma *
			    (resistance + MT6350_MODEL_R_FG_MOHM), 1000);

	return div_s64(compensate + 5, 10);
}

static inline void mt6350_model_reset(struct mt6350_battery_model *m, int soc)
{
	m->car1_01mah = 0;
	m->car2_01mah = 0;
	m->dod0 = 100 - clamp(soc, 0, 100);
	m->dod1 = m->dod0;
	m->dod2 = m->dod0;
	m->dod3 = m->dod0;
	/* battery_meter_reset leaves both observer OCV/resistance states in
	 * place. The next sample updates them from the rebased DOD/car.
	 */
}

static inline void mt6350_model_init(struct mt6350_battery_model *m,
				     int seed_mv, int temp_c, int initial_soc)
{
	memset(m, 0, sizeof(*m));
	mt6350_model_build_profile(m, temp_c);
	/* The factory chooses temperature Qmax at initialization, separately
	 * from the nameplate charge-full-design value exposed to userspace.
	 */
	m->qmax_mah = mt6350_model_qmax(temp_c);
	if (initial_soc < 0)
		initial_soc = mt6350_model_soc_by_voltage(m, seed_mv);
	mt6350_model_reset(m, initial_soc);
	m->ocv1_mv = mt6350_model_voltage_by_soc(m, 100 - m->dod0);
	m->ocv2_mv = m->ocv1_mv;
	m->r1_mohm = mt6350_model_resistance(m, seed_mv);
	m->r2_mohm = m->r1_mohm;
}

static inline int mt6350_model_car_dod(const struct mt6350_battery_model *m,
				      s64 car_01mah)
{
	s64 dod = m->dod0 + div_s64(car_01mah * 10, m->qmax_mah);

	return clamp_t(s64, dod, 0, 100);
}

static inline void mt6350_model_step(struct mt6350_battery_model *m,
				     int vbat_mv, int temp_c, int elapsed_sec)
{
	mt6350_model_update_profile(m, temp_c);
	m->current1_01ma = div_s64(((s64)m->ocv1_mv - vbat_mv) * 10000,
				 m->r1_mohm);
	m->current2_01ma = div_s64(((s64)m->ocv2_mv - vbat_mv) * 10000,
				 m->r2_mohm);
	/* Preserve factory whole-second, 0.1 mAh per-step truncation. */
	m->car1_01mah += div_s64((s64)m->current1_01ma * elapsed_sec, 3600);
	m->car2_01mah += div_s64((s64)m->current2_01ma * elapsed_sec, 3600);
	m->dod1 = mt6350_model_car_dod(m, m->car1_01mah);
	m->dod2 = mt6350_model_car_dod(m, m->car2_01mah);
	m->ocv1_mv = vbat_mv + mt6350_model_imp_tracking(m, vbat_mv,
						     m->current2_01ma);
	m->dod3 = clamp(mt6350_model_dod_by_voltage(m, m->ocv1_mv), 0, 100);
	m->r1_mohm = mt6350_model_resistance(m, m->ocv1_mv);
	m->ocv2_mv = mt6350_model_voltage_by_soc(m, 100 - m->dod2);
	m->r2_mohm = mt6350_model_resistance(m, m->ocv2_mv);
}

static inline void mt6350_model_sleep(struct mt6350_battery_model *m,
				      int sleep_sec, int old_ocv_mv,
				      int new_ocv_mv, bool online)
{
	/* The factory assumes 4 mA while asleep. This is a fixed estimate,
	 * not measured current; it also truncates each sleep interval.
	 */
	if (sleep_sec > 1800 && !online && new_ocv_mv < old_ocv_mv) {
		m->dod0 = mt6350_model_dod_by_voltage(m, new_ocv_mv);
		m->ocv1_mv = new_ocv_mv;
		m->ocv2_mv = new_ocv_mv;
		m->car1_01mah = 0;
		m->car2_01mah = 0;
	} else {
		m->car1_01mah += div_s64((s64)40 * sleep_sec, 3600);
		m->car2_01mah += div_s64((s64)40 * sleep_sec, 3600);
	}
	/* The next normal sample updates DOD1/2 and observer resistances. */
}

static inline int mt6350_model_soc(const struct mt6350_battery_model *m)
{
	return 100 - m->dod2;
}

#endif
