/*
 * LK boot probe, 32-bit ARM flavour: a minimal "zImage" for LK's stock 32-bit path.
 * Writes "RUFUS-PROBE32 cpsr=.. r0=.. r1=.. r2=.. *r2=.." into the pstore console zone
 * (shown by the stock kernel as /sys/fs/pstore/console-ramoops), then resets through
 * TOPRGU. LK finds the appended DTB at the zImage "end" offset (0x2c), as for the stock kernel.
 *
 * The header is valid from the first byte on and size is updated after every field, so a
 * fault part-way still leaves a readable partial marker. r2 is only dereferenced when it is
 * a 4-byte aligned DRAM address.
 */
	.arm
	.section .text
	.globl _start
_start:
	.rept	7
	mov	r0, r0
	.endr
	b	entry
	.word	0				@ 0x20
	.word	0x016f2818			@ 0x24 zImage magic
	.word	0				@ 0x28 start
	.word	_end - _start			@ 0x2c end: the DTB follows
	.word	0x04030201			@ 0x30 endianness

entry:
	mov	r8, r0
	mov	r9, r1
	mov	r10, r2
	mrs	r11, cpsr

	@ Visible sign of life first: paint LK's framebuffer white (480x640x4 at 0x7fb10000;
	@ the ST7701S runs in video mode and scans it out continuously), then wait ~3 s.
	ldr	r0, =0x7fb10000
	ldr	r1, =(480 * 640 * 4)
	add	r1, r0, r1
	mvn	r2, #0
5:	str	r2, [r0], #4
	cmp	r0, r1
	blo	5b
	dsb	sy

	ldr	r4, =0x5449f000			@ pstore console zone
	add	r5, r4, #12			@ data
	mov	r6, #0				@ bytes written
	str	r6, [r4, #4]			@ start
	str	r6, [r4, #8]			@ size
	ldr	r0, =0x43474244			@ PERSISTENT_RAM_SIG
	str	r0, [r4]
	bl	flush

	adrl	r1, m_head
	bl	puts
	mov	r0, r11
	bl	puthex
	adrl	r1, m_r0
	bl	puts
	mov	r0, r8
	bl	puthex
	adrl	r1, m_r1
	bl	puts
	mov	r0, r9
	bl	puthex
	adrl	r1, m_r2
	bl	puts
	mov	r0, r10
	bl	puthex

	tst	r10, #3				@ dereference r2 only if aligned DRAM
	bne	4f
	cmp	r10, #0x40000000
	blo	4f
	cmp	r10, #0xc0000000
	bhs	4f
	adrl	r1, m_dtb
	bl	puts
	ldr	r0, [r10]			@ 0xedfe0dd0 if r2 points at an FDT
	bl	puthex
4:	adrl	r1, m_end
	bl	puts

	ldr	r2, =0xff00ff00			@ green: marker written
	bl	fill
	ldr	r0, =2000000			@ short wait (uncached: ~1 s)
6:	subs	r0, r0, #1
	bne	6b

	ldr	r0, =0x10007014			@ TOPRGU WDT_SWRST
	ldr	r1, =0x1209
	str	r1, [r0]
	ldr	r0, =2000000
7:	subs	r0, r0, #1
	bne	7b
	ldr	r2, =0xffff0000			@ red: still alive, the reset did not happen
	bl	fill
1:	wfe
	b	1b

fill:						@ paint the 480x640 XRGB framebuffer with r2
	ldr	r0, =0x7fb10000
	ldr	r1, =(480 * 640 * 4)
	add	r1, r0, r1
8:	str	r2, [r0], #4
	cmp	r0, r1
	blo	8b
	dsb	sy
	bx	lr

puts:						@ copy string at r1, then publish
	ldrb	r2, [r1], #1
	cmp	r2, #0
	beq	flush
	strb	r2, [r5, r6]
	add	r6, r6, #1
	b	puts

puthex:						@ r0 as 8 hex digits, then publish
	mov	r3, #28
2:	lsr	r2, r0, r3
	and	r2, r2, #0xf
	cmp	r2, #10
	addlo	r2, r2, #'0'
	addhs	r2, r2, #('a' - 10)
	strb	r2, [r5, r6]
	add	r6, r6, #1
	subs	r3, r3, #4
	bge	2b

flush:						@ size/start = bytes written; clean lines to DRAM
	str	r6, [r4, #4]
	str	r6, [r4, #8]
	dsb	sy
	mov	r0, r4
	add	r1, r5, r6
3:	mcr	p15, 0, r0, c7, c10, 1		@ DCCMVAC, harmless with caches off
	add	r0, r0, #32
	cmp	r0, r1
	bls	3b
	dsb	sy
	bx	lr

	.ltorg
m_head:	.asciz	"\nRUFUS-PROBE32 cpsr="
m_r0:	.asciz	" r0="
m_r1:	.asciz	" r1="
m_r2:	.asciz	" r2="
m_dtb:	.asciz	" *r2="
m_end:	.asciz	" END\n"
	.balign	4
_end:
