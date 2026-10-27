/* SPDX-License-Identifier: GPL-2.0-only */
/* Banked-register and L2 helpers based on MediaTek's 2016 MT6580 BSP. */
/* Copyright (c) 2016 MediaTek Inc. */
#include <linux/linkage.h>
#include <asm/assembler.h>

	.text
	.arm

/* CPU0 alone, IRQ/FIQ masked, Cortex-A7 L2 retained. The BSP flushes L1
 * but only cleans L2 in this mode. No stack access until both levels have
 * reached PoC, and no exclusive operation after ACTLR.SMP is cleared.
 */
ENTRY(mt6580_cache_retention_exit)
	push	{r4-r10, lr}
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #4
	mcr	p15, 0, r0, c1, c0, 0
	dsb
	isb
	bl	v7_flush_dcache_louis
	/* Complete L1 maintenance before L2 (Cortex-A7 erratum 814220). */
	dsb
	mov	r0, #2
	mcr	p15, 2, r0, c0, c0, 0
	isb
	mrc	p15, 1, r1, c0, c0, 0
	and	r2, r1, #7
	add	r2, r2, #4
	ubfx	r3, r1, #3, #10
	clz	r4, r3
	ubfx	r5, r1, #13, #15
1:
	mov	r6, r5
2:
	mov	r9, r6, lsl r2
	orr	r9, r9, r3, lsl r4
	orr	r9, r9, #2
	mcr	p15, 0, r9, c7, c10, 2
	subs	r6, r6, #1
	bge	2b
	subs	r3, r3, #1
	bge	1b
	mov	r0, #0
	mcr	p15, 2, r0, c0, c0, 0
	dsb
	isb
	clrex
	mrc	p15, 0, r0, c1, c0, 1
	bic	r0, r0, #(1 << 6)
	mcr	p15, 0, r0, c1, c0, 1
	isb
	dsb
	pop	{r4-r10, pc}
ENDPROC(mt6580_cache_retention_exit)

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
