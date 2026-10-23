# SPDX-License-Identifier: GPL-2.0
# GPL Arm Utgard r8p0 core, Linux 6.18 / MT6580 platform integration.
obj-$(CONFIG_MALI400) += mali.o

mali-y := \
	linux/mali_osk_atomics.o \
	linux/mali_osk_irq.o \
	linux/mali_osk_wq.o \
	linux/mali_osk_locks.o \
	linux/mali_osk_wait_queue.o \
	linux/mali_osk_low_level_mem.o \
	linux/mali_osk_math.o \
	linux/mali_osk_memory.o \
	linux/mali_osk_misc.o \
	linux/mali_osk_mali.o \
	linux/mali_osk_notification.o \
	linux/mali_osk_time.o \
	linux/mali_osk_timers.o \
	linux/mali_osk_bitmap.o \
	linux/mali_memory.o \
	linux/mali_memory_os_alloc.o \
	linux/mali_memory_external.o \
	linux/mali_memory_block_alloc.o \
	linux/mali_memory_swap_alloc.o \
	linux/mali_memory_manager.o \
	linux/mali_memory_virtual.o \
	linux/mali_memory_util.o \
	linux/mali_memory_cow.o \
	linux/mali_memory_defer_bind.o \
	linux/mali_ukk_mem.o \
	linux/mali_ukk_gp.o \
	linux/mali_ukk_pp.o \
	linux/mali_ukk_core.o \
	linux/mali_ukk_soft_job.o \
	linux/mali_ukk_timeline.o \
	common/mali_kernel_core.o \
	linux/mali_kernel_linux.o \
	common/mali_session.o \
	linux/mali_device_pause_resume.o \
	common/mali_kernel_vsync.o \
	linux/mali_ukk_vsync.o \
	linux/mali_kernel_sysfs.o \
	common/mali_mmu.o \
	common/mali_mmu_page_directory.o \
	common/mali_mem_validation.o \
	common/mali_hw_core.o \
	common/mali_gp.o \
	common/mali_pp.o \
	common/mali_pp_job.o \
	common/mali_gp_job.o \
	common/mali_soft_job.o \
	common/mali_scheduler.o \
	common/mali_executor.o \
	common/mali_group.o \
	common/mali_dlbu.o \
	common/mali_broadcast.o \
	common/mali_pm.o \
	common/mali_pmu.o \
	common/mali_user_settings_db.o \
	common/mali_kernel_utilization.o \
	common/mali_control_timer.o \
	common/mali_l2_cache.o \
	common/mali_timeline.o \
	common/mali_timeline_fence_wait.o \
	common/mali_timeline_sync_fence.o \
	common/mali_spinlock_reentrant.o \
	common/mali_pm_domain.o \
	linux/mali_osk_pm.o \
	linux/mali_pmu_power_up_down.o \
	__malidrv_build_info.o \
	platform/mt6580-modern/platform.o \
	linux/mali_memory_dma_buf.o \
	linux/mali_memory_secure.o \
	linux/mali_sync.o \
	linux/mali_internal_sync.o \
	linux/mali_dma_fence.o

ccflags-y += -I$(src) -I$(src)/include -I$(src)/common -I$(src)/linux
ccflags-y += -I$(src)/linux/license/gpl -I$(src)/platform/mt6580-modern
ccflags-y += -include $(src)/linux/mali_compat_618.h
# The original DDK declares several internal entry points only at their use sites.
ccflags-y += -Wno-missing-prototypes -Wno-declaration-after-statement -Wno-date-time
ccflags-y += -DCONFIG_MALI400=1 -DCONFIG_MALI_DT=1
ccflags-y += -DCONFIG_MALI_DMA_BUF_FENCE=1 -DCONFIG_MALI_DMA_BUF_MAP_ON_ATTACH=1
ccflags-y += -DHAVE_UNLOCKED_IOCTL -DMALI_STATE_TRACKING=1
ccflags-y += -DMALI_OS_MEMORY_KERNEL_BUFFER_SIZE_IN_MB=16
ccflags-y += -DUSING_GPU_UTILIZATION=1 -DMALI_ENABLE_CPU_CYCLES=0
ccflags-y += -DMALI_UPPER_HALF_SCHEDULING
ccflags-y += -DMALI_PP_SCHEDULER_FORCE_NO_JOB_OVERLAP=0
ccflags-y += -DMALI_PP_SCHEDULER_KEEP_SUB_JOB_STARTS_ALIGNED=0
ccflags-y += -DMALI_PP_SCHEDULER_FORCE_NO_JOB_OVERLAP_BETWEEN_APPS=0
ccflags-y += -DSVN_REV_STRING=\"r8p0-00dev0-linux618-mt6580\"
ccflags-$(CONFIG_MALI400_DEBUG) += -DDEBUG
