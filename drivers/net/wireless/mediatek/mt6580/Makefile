# SPDX-License-Identifier: GPL-2.0-only
# MT6580 integrated AHB WLAN; the original _HIF_SDIO flag names its HIF protocol.
ccflags-y += -DLINUX -DMT6628 -D_HIF_SDIO=1 -DDBG=0
ccflags-y += -DCFG_SUPPORT_AGPS_ASSIST=0 -DCFG_SUPPORT_TSF_USING_BOOTTIME=1
ccflags-y += -DCFG_SUPPORT_WAPI=0 -DCFG_SUPPORT_MCC=0 -DCFG_SUPPORT_XLOG=0
ccflags-y += -DCFG_SUPPORT_AEE=0 -DCFG_SUPPORT_HOTSPOT_2_0=0 -DCFG_HS20_DEBUG=0
ccflags-y += -DCFG_P2P_LEGACY_COEX_REVISE=0 -DCFG_ENABLE_GTK_FRAME_FILTER=0 -DCFG_SUPPORT_MET_PROFILING=0 -DCFG_TC1_FEATURE=0 -DCFG_SRAM_SIZE_OPTION=1
ccflags-y += -I$(src)/os -I$(src)/os/linux/include -I$(src)/os/linux/hif/ahb/include
ccflags-y += -I$(src)/include -I$(src)/include/nic -I$(src)/include/mgmt
# Imported source predates the kernel's format/enum warning policy.
ccflags-y += -Wno-error -Wno-typedef-redefinition -Wno-enum-conversion

obj-y += mt6580_wlan.o
mt6580_wlan-y := common/dump.o common/wlan_lib.o common/wlan_oid.o \
 nic/nic.o nic/nic_tx.o nic/nic_rx.o nic/nic_pwr_mgt.o nic/cmd_buf.o \
 nic/que_mgt.o nic/nic_cmd_event.o \
 os/linux/gl_init.o os/linux/gl_kal.o os/linux/gl_wext.o \
 os/linux/gl_wext_priv.o os/linux/gl_rst.o os/linux/gl_cfg80211.o os/linux/platform.o \
 os/linux/hif/ahb/ahb.o \
 mgmt/ais_fsm.o mgmt/aaa_fsm.o mgmt/assoc.o mgmt/auth.o mgmt/bss.o \
 mgmt/cnm.o mgmt/cnm_timer.o mgmt/cnm_mem.o mgmt/hem_mbox.o mgmt/mib.o \
 mgmt/privacy.o mgmt/rate.o mgmt/rlm.o mgmt/rlm_domain.o mgmt/rlm_obss.o \
 mgmt/rlm_protection.o mgmt/rsn.o mgmt/saa_fsm.o mgmt/scan.o mgmt/scan_fsm.o \
 mgmt/sec_fsm.o mgmt/swcr.o mgmt/roaming_fsm.o mgmt/hs20.o mgmt/stats.o
