/*
 *
 * FocalTech TouchScreen driver.
 *
 * Copyright (c) 2012-2018, Focaltech Ltd. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/*****************************************************************************
*
* File Name: focaltech_core.h

* Author: Focaltech Driver Team
*
* Created: 2016-08-08
*
* Abstract:
*
* Reference:
*
*****************************************************************************/

#ifndef __LINUX_FOCALTECH_CORE_H__
#define __LINUX_FOCALTECH_CORE_H__
/*****************************************************************************
* 1.Included header files
*****************************************************************************/
#include <linux/syscalls.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/i2c.h>
#include <linux/vmalloc.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <mach/irqs.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/rtpm_prio.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include "tpd.h"
#include "focaltech_common.h"

/*****************************************************************************
* Private constant and macro definitions using #define
*****************************************************************************/
#define FTS_MAX_POINTS_SUPPORT              10 /* constant value, can't be changed */
#define FTS_MAX_ID                          0x0A
#define FTS_ONE_TCH_LEN                     6
#define FTS_FACE_DETECT_POS                 1
#define FTS_TOUCH_X_H_POS                   3
#define FTS_TOUCH_X_L_POS                   4
#define FTS_TOUCH_Y_H_POS                   5
#define FTS_TOUCH_Y_L_POS                   6
#define FTS_TOUCH_PRE_POS                   7
#define FTS_TOUCH_AREA_POS                  8
#define FTS_TOUCH_EVENT_POS                 3
#define FTS_TOUCH_ID_POS                    5
#define FTS_TOUCH_POINT_NUM                 2
#define FTS_TOUCH_XY_POS                    7
#define FTS_TOUCH_MISC                      8
#define FTS_COORDS_ARR_SIZE                 4
#define FT_FW_NAME_MAX_LEN                  50

#define FTS_TOUCH_DOWN                      0
#define FTS_TOUCH_UP                        1
#define FTS_TOUCH_CONTACT                   2
#define EVENT_DOWN(flag)                    ((FTS_TOUCH_DOWN == flag) || (FTS_TOUCH_CONTACT == flag))
#define EVENT_UP(flag)                      (FTS_TOUCH_UP == flag)
#define EVENT_NO_DOWN(data)                 (!data->point_num)
#define KEY_EN                              (tpd_dts_data.use_tpd_button)
#define TOUCH_IS_KEY(y, key_y)              (y > key_y)

#ifndef RTPM_PRIO_TPD
#define RTPM_PRIO_TPD                       0x04
#endif

#define FTX_MAX_COMPATIBLE_TYPE             4

/*****************************************************************************
* Private enumerations, structures and unions using typedef
*****************************************************************************/
/*touch event info*/
struct ts_event {
    int x; /*x coordinate */
    int y; /*y coordinate */
    int p; /* pressure */
    int flag; /* touch event flag: 0 -- down; 1-- up; 2 -- contact */
    int id;   /*touch ID */
    int area;
};
struct fts_ts_data {
    struct i2c_client *client;
    struct input_dev *input_dev;
    struct ts_ic_info ic_info;
    struct workqueue_struct *ts_workqueue;
    struct work_struct fwupg_work;
    struct delayed_work esdcheck_work;
    struct delayed_work prc_work;
    struct task_struct *thread_tpd;
    spinlock_t irq_lock;
    struct mutex report_mutex;
    int irq;
    bool suspended;
    bool fw_loading;
    bool irq_disabled;
    bool power_disabled;
    /* multi-touch */
    struct ts_event *events;
    u8 *point_buf;
    int pnt_buf_size;
    int touchs;
    bool key_down;
    int touch_point;
    int point_num;
    struct proc_dir_entry *proc;
    u8 proc_opmode;
};

/*****************************************************************************
* Global variable or extern global variabls/functions
*****************************************************************************/
extern struct tpd_device *tpd;
extern unsigned int tpd_rst_gpio_number;
extern struct fts_ts_data *fts_data;

/* i2c communication*/
int fts_i2c_write_reg(struct i2c_client *client, u8 regaddr, u8 regvalue);
int fts_i2c_read_reg(struct i2c_client *client, u8 regaddr, u8 *regvalue);
int fts_i2c_read(struct i2c_client *client, char *writebuf, int writelen, char *readbuf, int readlen);
int fts_i2c_write(struct i2c_client *client, char *writebuf, int writelen);
void fts_i2c_hid2std(struct i2c_client *client);
int fts_i2c_init(void);
int fts_i2c_exit(void);

/* Gesture functions */
#if FTS_GESTURE_EN
int fts_gesture_init(struct fts_ts_data *ts_data);
int fts_gesture_exit(struct i2c_client *client);
void fts_gesture_recovery(struct i2c_client *client);
int fts_gesture_readdata(struct fts_ts_data *ts_data);
int fts_gesture_suspend(struct i2c_client *i2c_client);
int fts_gesture_resume(struct i2c_client *client);
#endif

/* Apk and functions */
#if FTS_APK_NODE_EN
int fts_create_apk_debug_channel(struct fts_ts_data *);
void fts_release_apk_debug_channel(struct fts_ts_data *);
#endif

/* ADB functions */
#if FTS_SYSFS_NODE_EN
int fts_create_sysfs(struct i2c_client *client);
int fts_remove_sysfs(struct i2c_client *client);
#endif

/* ESD */
#if FTS_ESDCHECK_EN
int fts_esdcheck_init(struct fts_ts_data *ts_data);
int fts_esdcheck_exit(struct fts_ts_data *ts_data);
int fts_esdcheck_switch(bool enable);
int fts_esdcheck_proc_busy(bool proc_debug);
int fts_esdcheck_set_intr(bool intr);
int fts_esdcheck_suspend(void);
int fts_esdcheck_resume(void);
#endif


/* Point Report Check*/
#if FTS_POINT_REPORT_CHECK_EN
int fts_point_report_check_init(struct fts_ts_data *ts_data);
int fts_point_report_check_exit(struct fts_ts_data *ts_data);
void fts_prc_queue_work(struct fts_ts_data *ts_data);
#endif

/* FW upgrade */
int fts_upgrade_bin(struct i2c_client *client, char *fw_name, bool force);
int fts_fwupg_init(struct fts_ts_data *ts_data);
int fts_fwupg_exit(struct fts_ts_data *ts_data);

/* Other */
int fts_reset_proc(int hdelayms);
int fts_wait_tp_to_valid(struct i2c_client *client);
void fts_tp_state_recovery(struct i2c_client *client);
int fts_ex_mode_init(struct i2c_client *client);
int fts_ex_mode_exit(struct i2c_client *client);
int fts_ex_mode_recovery(struct i2c_client *client);

void fts_irq_disable(void);
void fts_irq_enable(void);

#if FTS_PSENSOR_EN
int fts_proximity_init(void);
int fts_proximity_exit(struct i2c_client *client);
int fts_proximity_readdata(struct i2c_client *client);
int fts_proximity_suspend(void);
int fts_proximity_resume(void);
int fts_proximity_recovery(struct i2c_client *client);
#endif

#endif /* __LINUX_FOCALTECH_CORE_H__ */
