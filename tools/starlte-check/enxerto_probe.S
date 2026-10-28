/* Test payload entry: capture the actual Enxerto handoff without using a stack. */
	.text
	.globl _start
_start:
	mov	x9, #0x4ff00000
	mov	x1, #0x4558
	str	x1, [x9, #0]
	str	x0, [x9, #8]
	mrs	x1, CurrentEL
	str	x1, [x9, #16]
	mrs	x1, sctlr_el1
	str	x1, [x9, #24]
	mrs	x1, cntfrq_el0
	str	x1, [x9, #32]
	mrs	x1, tcr_el1
	str	x1, [x9, #40]
	mrs	x1, ttbr0_el1
	str	x1, [x9, #48]
	mrs	x1, daif
	str	x1, [x9, #56]
	dsb	sy
	mov	x9, #0x09000000
	adr	x1, message
1:
	ldrb	w2, [x1], #1
	cbz	w2, 2f
	str	w2, [x9]
	b	1b
2:
	wfe
	b	2b
message:
	.asciz	"ENXERTO HANDOFF\n"
