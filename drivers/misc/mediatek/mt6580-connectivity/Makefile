# SPDX-License-Identifier: GPL-2.0-only
# Core/STP sources: OpenWatchProject MT6580, commit 65340b8dd12c.
obj-$(CONFIG_MT6580_CONSYS) += mt6580-consys.o
ccflags-y += -I$(src)/include -I$(src)/core/include -I$(src)/linux/include -I$(src)/linux/pri/include
ccflags-y += -DWMT_PLAT_ALPS -DWMT_CREATE_NODE_DYNAMIC=1 -DCFG_TC1_FEATURE=0 -DCFG_WMT_LTE_COEX_HANDLING=0 -DCONFIG_ARCH_MT6580
ccflags-y += -DCFG_FUNC_WIFI_SUPPORT=1 -DCFG_FUNC_BT_SUPPORT=0 -DCFG_FUNC_FM_SUPPORT=0 -DCFG_FUNC_GPS_SUPPORT=0 -DCFG_FUNC_ANT_SUPPORT=0
ccflags-y += -Wno-comment -Wno-undef -Wno-unused-function -Wno-unused-variable -Wno-missing-prototypes -Wno-format -Wno-pointer-sign
mt6580-consys-y := core/wmt_core.o core/wmt_ctrl.o core/wmt_func.o core/wmt_ic_soc.o core/wmt_lib.o core/wmt_conf.o
mt6580-consys-y += core/btm_core.o core/dbg_core.o core/psm_core.o core/stp_core.o
mt6580-consys-y += linux/pri/stp_btif.o linux/pri/stp_dbg.o linux/pri/stp_exp.o linux/pri/wmt_dev.o linux/pri/wmt_exp.o
mt6580-consys-y += linux/pub/osal.o linux/pub/bgw_desense.o linux/pub/wmt_chrdev_wifi.o
mt6580-consys-y += platform.o btif.o
