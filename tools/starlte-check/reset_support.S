/* Test-owned service boundaries around the complete production start.s. */
#include "assym.s"
	.text
	.globl _set_mmu_ttb
_set_mmu_ttb:
	msr TTBR0_EL1, x0
	isb
	ret
#if !TEST_FINAL_FB
	.globl _phystokv
_phystokv:
	sub x0, x0, x23
	add x0, x0, x22
	ret
#endif
	.globl _arm_init_cpu
	.globl _arm_init_idle_cpu
_arm_init_cpu:
	mov x18, #0x5354
	b Lwarm_report
_arm_init_idle_cpu:
	mov x18, #0x4944
Lwarm_report:
	adrp x9, _test_warm_reports@page
	add x9, x9, _test_warm_reports@pageoff
	ldr w10, [x0, CPU_NUMBER_GS]
	add x9, x9, x10, lsl #7
	str x18, [x9]
	str x0, [x9, #8]
	mrs x1, CurrentEL
	str x1, [x9, #16]
	mrs x1, SCTLR_EL1
	str x1, [x9, #24]
	mrs x1, TCR_EL1
	str x1, [x9, #32]
	mrs x1, TTBR0_EL1
	str x1, [x9, #40]
	mrs x1, TTBR1_EL1
	str x1, [x9, #48]
	mrs x1, TPIDR_EL0
	str x1, [x9, #56]
	mov x1, sp
	str x1, [x9, #64]
	msr SPSel, #1
	mov x1, sp
	msr SPSel, #0
	str x1, [x9, #72]
	mrs x1, VBAR_EL1
	str x1, [x9, #80]
	mrs x1, TPIDRRO_EL0
	str x1, [x9, #88]
	mrs x1, MPIDR_EL1
	and x1, x1, #0xffff
	str x1, [x9, #96]
	str x26, [x9, #104]
	mrs x1, TPIDR_EL1
	str x1, [x9, #112]
	dsb sy
	mov x1, #0x5041
	str x1, [x9, #120]
	dsb sy
	.globl _test_warm_done
_test_warm_done:
	wfe
	b _test_warm_done

	/* QEMU's generic loader sets PCs but leaves secondary CPUs powered off.
	 * This test-owned firmware caller starts them before CPU0 enters reset.
	 * virt uses HVC at EL1, SMC at EL2 (with secure=off).
	 */
	.globl _test_warm_boot_entry
_test_warm_boot_entry:
	mrs x21, CurrentEL
	adrp x20, _sweet_secondary_entry@page
	add x20, x20, _sweet_secondary_entry@pageoff
	cmp x21, #8
	b.ne 1f
	adrp x20, _test_secondary_affinity_entry@page
	add x20, x20, _test_secondary_affinity_entry@pageoff
1:
	adrp x22, _test_psci_results@page
	add x22, x22, _test_psci_results@pageoff
	mov x19, #1
2:
	mov x0, #3
	movk x0, #0xc400, lsl #16
	mov x1, x19
	mov x2, x20
	mov x3, #0xc0de
	cmp x21, #8
	b.eq 3f
	hvc #0
	b 4f
3:
	smc #0
4:
	str x0, [x22, x19, lsl #3]
	cbnz x0, 5f
	add x19, x19, #1
	cmp x19, #TEST_CPU_COUNT
	b.lo 2b
	mov x0, #0x5041
	str x0, [x22]
	dsb sy
	br x20
5:
	b .

	/* EL2 fixture only: virtualize Aff1:Aff0 as 0..3,0x100..0x103.
	 * The production secondary entry then selects CPU data by that MPIDR.
	 */
	.globl _test_secondary_affinity_entry
_test_secondary_affinity_entry:
	mrs x1, MPIDR_EL1
	and x2, x1, #3
	ubfx x3, x1, #2, #1
	orr x2, x2, x3, lsl #8
	bfi x1, x2, #0, #16
	msr VMPIDR_EL2, x1
	isb
	b _sweet_secondary_entry

	.globl _arm_init
_arm_init:
	adrp x9, _test_report@page
	add x9, x9, _test_report@pageoff
	mov x1, #0x5253
	str x1, [x9]
	str x0, [x9, #8]
	mrs x1, CurrentEL
	str x1, [x9, #16]
	mrs x1, SCTLR_EL1
	str x1, [x9, #24]
	mrs x1, TCR_EL1
	str x1, [x9, #32]
	mrs x1, TTBR0_EL1
	str x1, [x9, #40]
	mrs x1, TTBR1_EL1
	str x1, [x9, #48]
	ldr x1, [x0, BA_VIDEO_BASE]
	str x1, [x9, #56]
	cbz x1, 1f
	mov w2, #0x3344
	movk w2, #0x1122, lsl #16
	str w2, [x1]
	ldr x2, [x0, BA_VIDEO_ROWBYTES]
	ldr x3, [x0, BA_VIDEO_HEIGHT]
	madd x1, x2, x3, x1
	sub x1, x1, #4
	mov w2, #0x7788
	movk w2, #0x5566, lsl #16
	str w2, [x1]
1:
	ldr x1, [x0, BA_BOOT_FLAGS]
	str x1, [x9, #64]
#if TEST_FINAL_FB
	bl _test_final_framebuffer
	adrp x9, _test_report@page
	add x9, x9, _test_report@pageoff
#endif
	dsb sy
	mov x1, #0x5041
	str x1, [x9, #72]
	dsb sy
	.globl _test_done
_test_done:
	wfe
	b _test_done
	.p2align 11
	.globl _ExceptionVectorsBase
_ExceptionVectorsBase:
	.rept 16
	b .
	.p2align 7
	.endr
	.data
	.globl _const_boot_args
_const_boot_args:
	.space BA_SIZE
	.globl _cpu_ttep
_cpu_ttep:
	.quad 0
	.globl _test_report
_test_report:
	.space 80
	.globl _test_cpu_entries
_test_cpu_entries:
	.space TEST_CPU_COUNT * cdeSize_NUM
	.globl _test_cpu_data
_test_cpu_data:
	.space TEST_CPU_COUNT * cdSize_NUM
	.globl _test_warm_reports
_test_warm_reports:
	.space TEST_CPU_COUNT * 128
	.globl _test_psci_results
_test_psci_results:
	.rept TEST_CPU_COUNT
	.quad -1
	.endr
	.globl _test_kernel_root
	.zerofill __DATA,__bss,_test_kernel_root,0x4000,14
	.globl _test_cpu_stacks
	.zerofill __DATA,__bss,_test_cpu_stacks,0x40000,14
#if !TEST_FINAL_FB
	.globl _bootstrap_pagetables
	.zerofill __DATA,__bss,_bootstrap_pagetables,0x20000,14
#endif
	.p2align 14
	.space 0x4000
	.globl _excepstack_top
_excepstack_top:
	.space 0x4000
	.globl _intstack_top
_intstack_top:
