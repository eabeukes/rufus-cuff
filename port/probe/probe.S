/*
 * LK boot probe, arm64 flavour: a minimal "Image" for LK's 64-bit path.
 * Progress on screen (LK's framebuffer, 480x640 XRGB at 0x7fb10000, scanned out in video
 * mode): white = entered, green = marker written, red = the reset did not happen.
 * Marker "RUFUS-PROBE64 el=.. x0=.. x1=.. x2=.." goes to the pstore console zone
 * (0x5449f000) and, raw, to 0x54480000 inside the pstore dump area (read via /dev/mem).
 */
	.section .text
	.globl _start
_start:
	b	entry				// code0
	.long	0				// code1
	.quad	0				// text_offset
	.quad	_end - _start			// image_size (build pads and patches this)
	.quad	0xa				// flags: LE, 4K pages, place anywhere
	.quad	0, 0, 0				// reserved
	.ascii	"ARM\x64"			// magic
	.long	0				// reserved

entry:
	mov	x20, x0
	mov	x21, x1
	mov	x22, x2
	mrs	x24, CurrentEL

	mov	w2, #0xffffffff			// white: entered
	bl	fill

	ldr	x10, =0x5449f000		// pstore console zone
	add	x11, x10, #12
	mov	x12, #0
	str	w12, [x10, #4]
	str	w12, [x10, #8]
	ldr	w0, =0x43474244
	str	w0, [x10]

	adr	x1, m_head
	bl	puts
	mov	x0, x24
	bl	puthex
	adr	x1, m_x0
	bl	puts
	mov	x0, x20
	bl	puthex
	adr	x1, m_x1
	bl	puts
	mov	x0, x21
	bl	puthex
	adr	x1, m_x2
	bl	puts
	mov	x0, x22
	bl	puthex
	adr	x1, m_end
	bl	puts
	str	w12, [x10, #4]			// start
	str	w12, [x10, #8]			// size

	ldr	x13, =0x54480000		// raw copy for /dev/mem
	ldr	x0, =0x5546555220202020		// "    RUFU" little-endian tag
	str	x0, [x13], #8
	mov	x14, #0
2:	ldrb	w0, [x11, x14]
	strb	w0, [x13, x14]
	add	x14, x14, #1
	cmp	x14, x12
	b.lo	2b
	dsb	sy

	ldr	w2, =0xff00ff00			// green: marker written
	bl	fill
	ldr	x0, =2000000
3:	subs	x0, x0, #1
	b.ne	3b

	ldr	x0, =0x10007014			// TOPRGU WDT_SWRST
	mov	w1, #0x1209
	str	w1, [x0]
	ldr	x0, =2000000
4:	subs	x0, x0, #1
	b.ne	4b
	ldr	w2, =0xffff0000			// red: the reset did not happen
	bl	fill
1:	wfe
	b	1b

fill:						// paint the framebuffer with w2
	ldr	x0, =0x7fb10000
	ldr	x1, =(0x7fb10000 + 480 * 640 * 4)
5:	str	w2, [x0], #4
	cmp	x0, x1
	b.lo	5b
	dsb	sy
	ret

puts:						// copy string at x1 to x11+x12
	ldrb	w2, [x1], #1
	cbz	w2, 6f
	strb	w2, [x11, x12]
	add	x12, x12, #1
	b	puts
6:	ret

puthex:						// x0 as 16 hex digits
	mov	x3, #60
7:	lsr	x4, x0, x3
	and	x4, x4, #0xf
	cmp	x4, #10
	add	x5, x4, #'0'
	add	x6, x4, #('a' - 10)
	csel	x4, x5, x6, lo
	strb	w4, [x11, x12]
	add	x12, x12, #1
	subs	x3, x3, #4
	b.ge	7b
	ret

	.ltorg
m_head:	.asciz	"\nRUFUS-PROBE64 el="
m_x0:	.asciz	" x0="
m_x1:	.asciz	" x1="
m_x2:	.asciz	" x2="
m_end:	.asciz	" END\n"
	.balign	8
_end:
