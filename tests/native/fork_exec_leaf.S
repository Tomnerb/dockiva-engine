.text
.global _start
_start:
 ldr x9,[sp]
 cmp x9,#1
 b.ne fail
 mov x8,#172
 svc #0
 cmp x0,#1
 b.le fail
 mov x0,#0
 mov x1,#4096
 mov x2,#3
 mov x3,#0x22
 mov x4,#-1
 mov x5,#0
 mov x8,#222
 svc #0
 cmp x0,#0
 b.lt fail
 mov x10,#99
 stxr w9,x10,[x0]
 cbz w9,fail
 mov x0,#23
 mov x8,#94
 svc #0
fail:
 mov x0,#99
 mov x8,#94
 svc #0
