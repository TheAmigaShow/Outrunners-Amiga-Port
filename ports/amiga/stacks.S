	.text
	.globl _amiga_switch_stacks
_amiga_switch_stacks:
	move.l 4(%sp),%a0
	move.l 8(%sp),%a1
#ifdef __mc68060__
	fmovem.x %fp2-%fp7,-(%sp)
	fmove.l %fpcr,-(%sp)
#endif
	movem.l %d2-%d7/%a2-%a6,-(%sp)
	move.l %sp,(%a0)
	move.l %a1,%sp
	movem.l (%sp)+,%d2-%d7/%a2-%a6
#ifdef __mc68060__
	fmove.l (%sp)+,%fpcr
	fmovem.x (%sp)+,%fp2-%fp7
#endif
	rts

	.globl _amiga_c2p_init
_amiga_c2p_init:
	move.l %d3,-(%sp)
	moveq #0,%d0
	move.w #320,%d0
	moveq #0,%d1
	move.w #224,%d1
	moveq #16,%d3
#ifdef __mc68060__
	move.l %d5,-(%sp)
	move.l #10240,%d5
	jsr _c2p1x1_8_c5_040_init
	move.l (%sp)+,%d5
#else
	jsr _c2p1x1_8_c5_030_init
#endif
	move.l (%sp)+,%d3
	rts

	.globl _amiga_c2p
_amiga_c2p:
	move.l 4(%sp),%a0
	move.l 8(%sp),%a1
#ifdef __mc68060__
	jmp _c2p1x1_8_c5_040
#else
	jmp _c2p1x1_8_c5_030
#endif
