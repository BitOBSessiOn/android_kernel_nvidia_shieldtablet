/*
 * kexec for arm64
 *
 * Copyright (C) Linaro.
 * Copyright (C) Huawei Futurewei Technologies.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kexec.h>

#include <asm/assembler.h>
#include <asm/kexec.h>
#include <asm/page.h>
#include <asm/sysreg.h>

#ifdef CONFIG_KEXEC_HARDBOOT
#define TEGRA_PMC_BASE                 0x7000E400
#endif

/*
 * arm64_relocate_new_kernel - Put a 2nd stage image in place and boot it.
 *
 * The memory that the old kernel occupies may be overwritten when coping the
 * new image to its final location.  To assure that the
 * arm64_relocate_new_kernel routine which does that copy is not overwritten,
 * all code and data needed by arm64_relocate_new_kernel must be between the
 * symbols arm64_relocate_new_kernel and arm64_relocate_new_kernel_end.  The
 * machine_kexec() routine will copy arm64_relocate_new_kernel to the kexec
 * control_code_page, a special page which has been set up to be preserved
 * during the copy operation.
 */
.globl arm64_relocate_new_kernel
arm64_relocate_new_kernel:

	/* Setup the list loop variables. */
	mov	x18, x1				/* x18 = kimage_start */
	mov	x17, x0				/* x17 = kimage_head */
	dcache_line_size x16, x0		/* x16 = dcache line size */
	mov	x15, xzr			/* x15 = segment start */
	mov	x14, xzr			/* x14 = entry ptr */
	mov	x13, xzr			/* x13 = copy dest */

	/* Clear the sctlr_el2 flags. */
	mrs	x0, CurrentEL
	cmp	x0, #CurrentEL_EL2
	b.ne	1f
	mrs	x0, sctlr_el2
	ldr	x1, =SCTLR_ELx_FLAGS
	bic	x0, x0, x1
	msr	sctlr_el2, x0
	isb
1:

	/* Check if the new image needs relocation. */
	cbz	x17, .Ldone
	tbnz	x17, IND_DONE_BIT, .Ldone

.Lloop:
	and	x12, x17, PAGE_MASK		/* x12 = addr */

	/* Test the entry flags. */
.Ltest_source:
	tbz	x17, IND_SOURCE_BIT, .Ltest_indirection

	/* Invalidate dest page to PoC. */
	mov     x0, x13
	add     x20, x0, #PAGE_SIZE
	sub     x1, x16, #1
	bic     x0, x0, x1
2:	dc      ivac, x0
	add     x0, x0, x16
	cmp     x0, x20
	b.lo    2b
	dsb     sy

	mov x20, x13
	mov x21, x12
	copy_page x20, x21, x0, x1, x2, x3, x4, x5, x6, x7

	/* dest += PAGE_SIZE */
	add	x13, x13, PAGE_SIZE
	b	.Lnext

.Ltest_indirection:
	tbz	x17, IND_INDIRECTION_BIT, .Ltest_destination

	/* ptr = addr */
	mov	x14, x12
	b	.Lnext

.Ltest_destination:
	tbz	x17, IND_DESTINATION_BIT, .Lnext

	mov	x15, x12

	/* dest = addr */
	mov	x13, x12

.Lnext:
	/* entry = *ptr++ */
	ldr	x17, [x14], #8

	/* while (!(entry & DONE)) */
	tbz	x17, IND_DONE_BIT, .Lloop

.Ldone:
	dsb	nsh
	ic	iallu
	dsb	nsh
	isb

#ifdef CONFIG_KEXEC_HARDBOOT
	ldr	x0, arm64_kexec_hardboot
	cbnz	x0, hardboot
#endif

	/* Start new image. */
	mov	x0, xzr
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x18

#ifdef CONFIG_KEXEC_HARDBOOT
hardboot:
	/* Stash boot arguments in hardboot page:
	 * 0: KEXEC_HB_PAGE_MAGIC
	 * 8: kexec_kimage_start
	 */
	ldr	x0, =KEXEC_HB_PAGE_ADDR
	str	x18, [x0, #8]
	ldr	x1, =KEXEC_HB_PAGE_MAGIC
	str	x1, [x0]

	ldr	x0, =TEGRA_PMC_BASE
	ldr     x1, [x0]
	orr     x1, x1, #0x10
	str     x1, [x0]
loop:	b	loop
#endif

.ltorg

.align 3	/* To keep the 64-bit values below naturally aligned. */

.Lcopy_end:
.org	KEXEC_CONTROL_PAGE_SIZE

/*
 * arm64_relocate_new_kernel_size - Number of bytes to copy to the
 * control_code_page.
 */
.globl arm64_relocate_new_kernel_size
arm64_relocate_new_kernel_size:
	.quad	.Lcopy_end - arm64_relocate_new_kernel

/*
 * arm64_kexec_hardboot - set to true if booting in hardboot mode
 */
.globl arm64_kexec_hardboot
arm64_kexec_hardboot:
	.quad	0x0
