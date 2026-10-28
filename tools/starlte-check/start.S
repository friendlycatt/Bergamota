/* Test-only startup/vector glue; the GIC and PSCI code is compiled from XNU. */
.section .text.boot, "ax"
.global _start
_start:
	mov x19, x0
	mov x0, #0
	bl setup_stack_vectors
	mov x0, x19
	bl test_main
	b .

.global test_secondary_entry
test_secondary_entry:
	mov x19, x0
	bl setup_stack_vectors
	mov x0, x19
	bl test_secondary
	b .

setup_stack_vectors:
	msr daifset, #0xf
	msr spsel, #1
	adrp x1, test_stacks
	add x1, x1, :lo12:test_stacks
	add x1, x1, x0, lsl #14
	add x1, x1, #0x4000
	mov sp, x1
	adrp x1, test_vectors
	add x1, x1, :lo12:test_vectors
	msr vbar_el1, x1
	isb
	ret

.macro vector slot
	.balign 0x80
	stp x29, x30, [sp, #-16]!
	mov x29, #\slot
	b exception
.endm
.balign 0x800
test_vectors:
vector 0
vector 1
vector 2
vector 3
vector 4
vector 5
vector 6
vector 7
vector 8
vector 9
vector 10
vector 11
vector 12
vector 13
vector 14
vector 15

exception:
	sub sp, sp, #240
	stp x0, x1, [sp, #0]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x19, [sp, #144]
	stp x20, x21, [sp, #160]
	stp x22, x23, [sp, #176]
	stp x24, x25, [sp, #192]
	stp x26, x27, [sp, #208]
	str x28, [sp, #224]
	mov x0, x29
	bl test_exception
	ldp x0, x1, [sp, #0]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x19, [sp, #144]
	ldp x20, x21, [sp, #160]
	ldp x22, x23, [sp, #176]
	ldp x24, x25, [sp, #192]
	ldp x26, x27, [sp, #208]
	ldr x28, [sp, #224]
	add sp, sp, #240
	ldp x29, x30, [sp], #16
	eret

.section .bss
.balign 16384
test_stacks:
.skip 8 * 16384
