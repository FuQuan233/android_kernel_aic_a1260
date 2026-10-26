/* SPDX-License-Identifier: GPL-2.0-only */
/* Banked-register and L2 helpers based on MediaTek's 2016 MT6580 BSP. */
/* Copyright (c) 2016 MediaTek Inc. */
#include <linux/linkage.h>
#include <asm/assembler.h>

	.text
	.arm

ENTRY(mt6580_cpu_resume)
	/* Restore writable L2 control before cpu_resume enables caches. */
	ldr_l	r0, mt6580_saved_l2ctlr
	mrc	p15, 1, r1, c9, c0, 2
	mov	r3, r1
	movw	r2, #0xffff
	movt	r2, #0x0003
	and	r0, r0, r2
	bic	r1, r1, r2
	orr	r0, r0, r1
	cmp	r0, r3
	mcrne	p15, 1, r0, c9, c0, 2
	dsb
	isb
	b	cpu_resume_arm
ENDPROC(mt6580_cpu_resume)

ENTRY(mt6580_l2_erratum_entry)
	cpsid	if
	ldr	r0, =0x10212000
	mov	r1, #0
	str	r1, [r0]
1:
	dsb
	isb
	wfi
	b	1b
ENDPROC(mt6580_l2_erratum_entry)

ENTRY(mt6580_save_banked)
	mrs	r2, cpsr
	cps	#0x1f
	stmia	r0!, {sp, lr}
	cps	#0x17
	str	sp, [r0], #4
	mrs	r3, spsr
	stmia	r0!, {r3, lr}
	cps	#0x1b
	str	sp, [r0], #4
	mrs	r3, spsr
	stmia	r0!, {r3, lr}
	cps	#0x12
	str	sp, [r0], #4
	mrs	r3, spsr
	stmia	r0!, {r3, lr}
	cps	#0x11
	str	sp, [r0], #4
	mrs	r3, spsr
	stmia	r0!, {r3, r8-r12, lr}
	msr	cpsr_cxsf, r2
	mrs	r3, spsr
	stmia	r0!, {r2, r3}
	dsb
	bx	lr
ENDPROC(mt6580_save_banked)

ENTRY(mt6580_restore_banked)
	mrs	r2, cpsr
	cps	#0x1f
	ldmia	r0!, {sp, lr}
	cps	#0x17
	ldr	sp, [r0], #4
	ldmia	r0!, {r3, lr}
	msr	spsr_fsxc, r3
	cps	#0x1b
	ldr	sp, [r0], #4
	ldmia	r0!, {r3, lr}
	msr	spsr_fsxc, r3
	cps	#0x12
	ldr	sp, [r0], #4
	ldmia	r0!, {r3, lr}
	msr	spsr_fsxc, r3
	cps	#0x11
	ldr	sp, [r0], #4
	ldmia	r0!, {r3, r8-r12, lr}
	msr	spsr_fsxc, r3
	msr	cpsr_cxsf, r2
	ldmia	r0!, {r2, r3}
	msr	spsr_fsxc, r3
	msr	cpsr_fsxc, r2
	dsb
	bx	lr
ENDPROC(mt6580_restore_banked)
