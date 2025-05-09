/*
 *
 * FocalTech fts TouchScreen driver.
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
* File Name: focaltech_upgrade_ft6426.c
*
* Author: Focaltech Driver Team
*
* Created: 2016-08-15
*
* Abstract:
*
* Reference:
*
*****************************************************************************/
/*****************************************************************************
* 1.Included header files
*****************************************************************************/
#include "../focaltech_core.h"
#include "../focaltech_flash.h"

#define FTS_REG_UPGRADE2                        0xBC
#define FTS_FLASH_STATUS_OK_FT6426              0xB002

/*****************************************************************************
* Static function prototypes
*****************************************************************************/
int fts_ft6426_reset_to_boot(struct i2c_client *client)
{
    int ret = 0;

    FTS_INFO("send 0xAA and 0x55 to FW, reset to boot environment");

    ret = fts_i2c_write_reg(client, FTS_REG_UPGRADE2, FTS_UPGRADE_AA);
    if (ret < 0) {
        FTS_ERROR("write FC=0xAA fail");
        return ret;
    }
    msleep(FTS_DELAY_FC_AA);

    ret = fts_i2c_write_reg(client, FTS_REG_UPGRADE2, FTS_UPGRADE_55);
    if (ret < 0) {
        FTS_ERROR("write FC=0x55 fail");
        return ret;
    }

    msleep(FTS_DELAY_UPGRADE_RESET);
    return 0;
}

/************************************************************************
* Name: fts_ft6426_check_flash_status
* Brief:
* Input: flash_status: correct value from tp
*        retries: read retry times
*        retries_delay: retry delay
* Output:
* Return: return true if flash status check pass, otherwise return false
***********************************************************************/
static bool fts_ft6426_check_flash_status(
    struct i2c_client *client,
    u16 flash_status,
    int retries,
    int retries_delay)
{
    int ret = 0;
    int i = 0;
    u8 cmd[4] = { 0 };
    u8 val[FTS_CMD_FLASH_STATUS_LEN] = { 0 };
    u16 read_status = 0;

    for (i = 0; i < retries; i++) {
        /* w 6a 00 00 00 r 2 */
        cmd[0] = FTS_CMD_FLASH_STATUS;
        ret = fts_i2c_read(client, cmd, 4, val, FTS_CMD_FLASH_STATUS_LEN);
        read_status = (((u16)val[0]) << 8) + val[1];

        if (flash_status == read_status) {
            /* FTS_DEBUG("[UPGRADE]flash status ok"); */
            return true;
        }
        FTS_DEBUG("flash status fail,ok:%04x read:%04x, retries:%d", flash_status, read_status, i);
        msleep(retries_delay);
    }

    return false;
}

/************************************************************************
* Name: fts_ft6426_erase
* Brief:
* Input:
* Output:
* Return: return 0 if success, otherwise return error code
***********************************************************************/
static int fts_ft6426_erase(struct i2c_client *client)
{
    int ret = 0;
    u8 cmd = 0;
    bool flag = false;

    FTS_INFO("**********erase app now**********");

    /*send to erase flash*/
    cmd = FTS_CMD_ERASE_APP;
    ret = fts_i2c_write(client, &cmd, 1);
    if (ret < 0) {
        FTS_ERROR("erase cmd fail");
        return ret;
    }
    msleep(500);

    /* read status 0xF0AA: success */
    flag = fts_ft6426_check_flash_status(client, FTS_FLASH_STATUS_OK_FT6426, 50, 100);
    if (!flag) {
        FTS_ERROR("ecc flash status check fail");
        return -EIO;
    }

    return 0;
}

/************************************************************************
* Name: fts_ft6426_write_app
* Brief:
* Input:
* Output:
* Return: return ecc if success, otherwise return error code
***********************************************************************/
static int fts_ft6426_write_app(struct i2c_client *client, u32 start_addr, u8 *buf, u32 len)
{
    int ret = 0;
    u32 i = 0;
    u32 j = 0;
    u32 packet_number = 0;
    u32 packet_len = 0;
    u32 addr = 0;
    u32 offset = 0;
    u32 remainder = 0;
    u8 packet_buf[FTS_FLASH_PACKET_LENGTH + FTS_CMD_WRITE_LEN] = { 0 };
    u8 ecc_in_host = 0;
    u8 cmd[4] = {0};
    u8 val[FTS_CMD_FLASH_STATUS_LEN] = { 0 };
    u16 read_status = 0;
    u16 wr_ok = 0;

    FTS_INFO( "**********write app to flash**********");

    if (NULL == buf) {
        FTS_ERROR("buf is null");
        return -EINVAL;
    }

    FTS_INFO("tp fw len=%d", len);
    packet_number = len / FTS_FLASH_PACKET_LENGTH;
    remainder = len % FTS_FLASH_PACKET_LENGTH;
    if (remainder > 0)
        packet_number++;
    packet_len = FTS_FLASH_PACKET_LENGTH;
    FTS_INFO("write fw,num:%d, remainder:%d", packet_number, remainder);

    packet_buf[0] = FTS_CMD_WRITE;
    for (i = 0; i < packet_number; i++) {
        offset = i * FTS_FLASH_PACKET_LENGTH;
        addr = start_addr + offset;
        packet_buf[1] = BYTE_OFF_16(addr);
        packet_buf[2] = BYTE_OFF_8(addr);
        packet_buf[3] = BYTE_OFF_0(addr);

        /* last packet */
        if ((i == (packet_number - 1)) && remainder)
            packet_len = remainder;

        packet_buf[4] = BYTE_OFF_8(packet_len);
        packet_buf[5] = BYTE_OFF_0(packet_len);

        for (j = 0; j < packet_len; j++) {
            packet_buf[FTS_CMD_WRITE_LEN + j] = buf[offset + j];
            ecc_in_host ^= packet_buf[FTS_CMD_WRITE_LEN + j];
        }

        ret = fts_i2c_write(client, packet_buf, packet_len + FTS_CMD_WRITE_LEN);
        if (ret < 0) {
            FTS_ERROR("[UPGRADE]app write fail");
            return ret;
        }
        mdelay(1);

        /* read status */
        wr_ok = FTS_FLASH_STATUS_OK_FT6426 + i + 1;
        for (j = 0; j < FTS_RETRIES_WRITE; j++) {
            cmd[0] = FTS_CMD_FLASH_STATUS;
            cmd[1] = 0x00;
            cmd[2] = 0x00;
            cmd[3] = 0x00;
            ret = fts_i2c_read(client, cmd , 4, val, FTS_CMD_FLASH_STATUS_LEN);
            read_status = (((u16)val[0]) << 8) + val[1];

            if (wr_ok == read_status) {
                break;
            }
            mdelay(FTS_RETRIES_DELAY_WRITE);
        }
    }

    return (int)ecc_in_host;
}

/************************************************************************
* Name: fts_ft6426_ecc_cal
* Brief:
* Input:
* Output:
* Return: return ecc if success, otherwise return error code
***********************************************************************/
static int fts_ft6426_ecc_cal(struct i2c_client *client)
{
    int ret = 0;
    u8 reg_val = 0;
    u8 cmd = 0;

    FTS_INFO("read out ecc");

    cmd = 0xcc;
    ret = fts_i2c_read(client, &cmd, 1, &reg_val, 1);
    if (ret < 0) {
        FTS_ERROR("read ft6426 ecc fail");
        return ret;
    }

    return reg_val;
}

/************************************************************************
* Name: fts_ft6426_upgrade
* Brief:
* Input:
* Output:
* Return: return 0 if success, otherwise return error code
***********************************************************************/
static int fts_ft6426_upgrade(struct i2c_client *client, u8 *buf, u32 len)
{
    int ret = 0;
    bool state = false;
    u32 start_addr = 0;
    int ecc_in_host = 0;
    int ecc_in_tp = 0;
    u32 fw_length = 0;

    if (NULL == buf) {
        FTS_ERROR("fw buf is null");
        return -EINVAL;
    }

    if ((len < 0x120) || (len > (60 * 1024))) {
        FTS_ERROR("fw buffer len(%x) fail", len);
        return -EINVAL;
    }

    fw_length = ((u32)buf[0x100] << 8) + buf[0x101];
    FTS_INFO("fw length is %d %d", fw_length, len);
    /* enter into upgrade environment */
    state = fts_fwupg_check_fw_valid(client);
    if (true == state) {
        ret = fts_ft6426_reset_to_boot(client);
        if (ret < 0) {
            FTS_ERROR("enter into bootloader fail");
            goto fw_reset;
        }
    } else if (upgrade_func_ft6426.read_boot_id_need_reset) {
        ret = fts_fwupg_reset_in_boot(client);
        if (ret < 0) {
            FTS_ERROR("reset before read boot id when fw invalid fail");
            return ret;
        }
    }

    state = fts_fwupg_check_state(client, FTS_RUN_IN_BOOTLOADER);
    if (!state) {
        FTS_ERROR("fw not in bootloader, fail");
        goto fw_reset;
    }

    ret = fts_ft6426_erase(client);
    if (ret < 0) {
        FTS_ERROR("erase cmd write fail");
        goto fw_reset;
    }

    /* write app */
    start_addr = upgrade_func_ft6426.appoff;
    ecc_in_host = fts_ft6426_write_app(client, start_addr, buf, fw_length);
    if (ecc_in_host < 0 ) {
        FTS_ERROR("lcd initial code write fail");
        goto fw_reset;
    }

    /* ecc */
    ecc_in_tp = fts_ft6426_ecc_cal(client);
    if (ecc_in_tp < 0 ) {
        FTS_ERROR("ecc read fail");
        goto fw_reset;
    }

    FTS_INFO("ecc in tp:%x, host:%x", ecc_in_tp, ecc_in_host);
    if (ecc_in_tp != ecc_in_host) {
        FTS_ERROR("ecc check fail");
        goto fw_reset;
    }

    FTS_INFO("upgrade success, reset to normal boot");
    ret = fts_fwupg_reset_in_boot(client);
    if (ret < 0) {
        FTS_ERROR("reset to normal boot fail");
    }

    msleep(200);
    return 0;

fw_reset:
    FTS_INFO("upgrade fail, reset to normal boot");
    ret = fts_fwupg_reset_in_boot(client);
    if (ret < 0) {
        FTS_ERROR("reset to normal boot fail");
    }
    return -EIO;
}

struct upgrade_func upgrade_func_ft6426 = {
    .ctype = {0x03, 0x04},
    .fwveroff = 0x010A,
    .fwcfgoff = 0x07B0,
    .appoff = 0x0000,
    .read_boot_id_need_reset = true,
    .pramboot_supported = false,
    .hid_supported = false,
    .upgrade = fts_ft6426_upgrade,
};
