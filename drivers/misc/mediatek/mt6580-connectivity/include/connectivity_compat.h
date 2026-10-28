/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6580_CONNECTIVITY_COMPAT_H
#define MT6580_CONNECTIVITY_COMPAT_H
#include <linux/sched/signal.h>
#include <linux/sched/types.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/printk.h>
#define DB_OPT_DEFAULT 0
#define DB_OPT_FTRACE 0
#define DB_OPT_DUMMY_DUMP 0
#define aee_kernel_warning(module, msg, ...) pr_warn("%s: %s", module, msg)
#define aee_kernel_warning_api(file, line, opt, module, msg, ...) pr_warn("%s: %s", module, msg)
#define aee_kernel_dal_show(message) pr_err("CONSYS firmware diagnostic: %s", message)
/* The upstream kernel has no MediaTek AEE daemon ABI. Retain the real
 * fault description in printk; STP's native netlink/dump path retains data. */
static inline void aed_combo_exception(const int *log, int log_size,
                                      const int *dump, int dump_size,
                                      const char *description)
{
    pr_err("CONSYS firmware fault: %s (dump bytes=%d)\n", description, dump_size);
}
struct wmt_timeval { time64_t tv_sec; long tv_usec; };
static inline void wmt_gettimeofday(struct wmt_timeval *value)
{
    struct timespec64 now;
    ktime_get_real_ts64(&now);
    value->tv_sec = now.tv_sec;
    value->tv_usec = now.tv_nsec / NSEC_PER_USEC;
}
#endif
