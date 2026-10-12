.text
.global _start
_start:
 adr x19,shared
 mov x0,#0xf00
 movk x0,#1,lsl #16
 adr x1,stack_end
 mov x8,#220
 svc #0
 cmp x0,#0
 b.lt fail
 cbz x0,worker
ready:
 ldr w9,[x19]
 cbnz w9,fork_now
 mov x8,#124
 svc #0
 b ready
fork_now:
 mov x9,#123
 msr tpidr_el0,x9
 fmov d8,#2.0
 mov x0,#17
 mov x1,#0
 mov x8,#220
 svc #0
 cmp x0,#0
 b.lt fail
 cbz x0,child
 mov x20,x0
 mov x0,x20
 add x1,x19,#12
 mov x2,#0
 mov x3,#0
 mov x8,#260
 svc #0
 cmp x0,x20
 b.ne fail
 ldr w9,[x19,#12]
 cmp w9,#0x700
 b.ne fail
 ldr w9,[x19,#8]
 cbnz w9,fail
 mov w9,#1
 str w9,[x19,#4]
 add x0,x19,#4
 mov x1,#129
 mov x2,#1
 mov x8,#98
 svc #0
 cmp x0,#1
 b.hi fail
joined:
 ldr w9,[x19,#8]
 cbnz w9,done
 mov x8,#124
 svc #0
 b joined
done:
 mov x0,#0
 mov x8,#94
 svc #0
worker:
 mov w9,#1
 str w9,[x19]
worker_wait:
 ldr w9,[x19,#4]
 cbnz w9,worker_exit
 add x0,x19,#4
 mov x1,#128
 mov x2,#0
 mov x3,#0
 mov x8,#98
 svc #0
 b worker_wait
worker_exit:
 mov w9,#1
 str w9,[x19,#8]
 mov x0,#0
 mov x8,#93
 svc #0
child:
 mrs x9,tpidr_el0
 cmp x9,#123
 b.ne fail
 fmov d9,#2.0
 fcmp d8,d9
 b.ne fail
 add x0,x19,#4
 mov x1,#129
 mov x2,#1
 mov x8,#98
 svc #0
 cbnz x0,fail
 mov w9,#2
 str w9,[x19,#8]
 mov x0,#7
 mov x8,#94
 svc #0
fail:
 mov x0,#99
 mov x8,#94
 svc #0
.bss
.balign 16
shared: .skip 32
stack: .skip 16384
stack_end:
