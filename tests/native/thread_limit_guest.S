/* 127 threads join the main thread (128 per process); the next clone fails with EAGAIN. */
.text
.global _start
_start:
 mov x19,#127
again:
 mov x0,#0xf00
 movk x0,#1,lsl #16
 adr x1,stack_end
 mov x8,#220
 svc #0
 cbz x0,spin
 cmp x0,#0
 b.lt fail
 subs x19,x19,#1
 b.ne again
 mov x0,#0xf00
 movk x0,#1,lsl #16
 adr x1,stack_end
 mov x8,#220
 svc #0
 cmn x0,#11
 b.ne fail
 mov x0,#0
 mov x8,#94
 svc #0
spin: b spin
fail:
 mov x0,#99
 mov x8,#94
 svc #0
.bss
.balign 16
stack: .skip 16384
stack_end:
