.text
.global _start
_start:
movz x1, #65529
movk x1, #65535, lsl #16
movk x1, #65535, lsl #32
movk x1, #65535, lsl #48
movz x2, #2
movz x3, #0
.inst 0x9ac20c2a
movz x11, #65533
movk x11, #65535, lsl #16
movk x11, #65535, lsl #32
movk x11, #65535, lsl #48
cmp x10, x11
b.ne fail
movz x1, #0
movk x1, #32768, lsl #48
movz x2, #65535
movk x2, #65535, lsl #16
movk x2, #65535, lsl #32
movk x2, #65535, lsl #48
movz x3, #0
.inst 0x9ac20c2a
movz x11, #0
movk x11, #32768, lsl #48
cmp x10, x11
b.ne fail
movz x1, #19
movz x2, #0
movz x3, #0
.inst 0x9ac2082a
movz x11, #0
cmp x10, x11
b.ne fail
movz x1, #65529
movk x1, #65535, lsl #16
movk x1, #65535, lsl #32
movk x1, #65535, lsl #48
movz x2, #2
movz x3, #0
.inst 0x1ac20c2a
movz x11, #65533
movk x11, #65535, lsl #16
cmp x10, x11
b.ne fail
movz x1, #65535
movk x1, #65535, lsl #16
movz x2, #2
movz x3, #3
.inst 0x9ba20c2a
movz x11, #1
movk x11, #2, lsl #32
cmp x10, x11
b.ne fail
movz x1, #65535
movk x1, #65535, lsl #16
movz x2, #2
movz x3, #3
.inst 0x9b220c2a
movz x11, #1
cmp x10, x11
b.ne fail
movz x1, #4
movz x2, #5
movz x3, #3
.inst 0x9ba28c2a
movz x11, #65519
movk x11, #65535, lsl #16
movk x11, #65535, lsl #32
movk x11, #65535, lsl #48
cmp x10, x11
b.ne fail
movz x1, #1
movz x2, #0
movz x3, #0
.inst 0x5ac0002a
movz x11, #0
movk x11, #32768, lsl #16
cmp x10, x11
b.ne fail
movz x1, #1800
movk x1, #1286, lsl #16
movk x1, #772, lsl #32
movk x1, #258, lsl #48
movz x2, #0
movz x3, #0
.inst 0xdac00c2a
movz x11, #513
movk x11, #1027, lsl #16
movk x11, #1541, lsl #32
movk x11, #2055, lsl #48
cmp x10, x11
b.ne fail
movz x1, #1800
movk x1, #1286, lsl #16
movk x1, #772, lsl #32
movk x1, #258, lsl #48
movz x2, #0
movz x3, #0
.inst 0xdac0042a
movz x11, #2055
movk x11, #1541, lsl #16
movk x11, #1027, lsl #32
movk x11, #513, lsl #48
cmp x10, x11
b.ne fail
movz x1, #0
movz x2, #0
movz x3, #0
.inst 0xdac0102a
movz x11, #64
cmp x10, x11
b.ne fail
movz x1, #65535
movk x1, #65535, lsl #16
movk x1, #65535, lsl #32
movk x1, #65535, lsl #48
movz x2, #0
movz x3, #0
.inst 0xdac0142a
movz x11, #63
cmp x10, x11
b.ne fail
movz x1, #3
movz x2, #65
movz x3, #0
.inst 0x9ac2202a
movz x11, #6
cmp x10, x11
b.ne fail
movz x1, #8
movz x2, #1
movz x3, #0
.inst 0x9ac2242a
movz x11, #4
cmp x10, x11
b.ne fail
movz x1, #65528
movk x1, #65535, lsl #16
movk x1, #65535, lsl #32
movk x1, #65535, lsl #48
movz x2, #1
movz x3, #0
.inst 0x9ac2282a
movz x11, #65532
movk x11, #65535, lsl #16
movk x11, #65535, lsl #32
movk x11, #65535, lsl #48
cmp x10, x11
b.ne fail
movz x1, #1
movz x2, #1
movz x3, #0
.inst 0x9ac22c2a
movz x11, #0
movk x11, #32768, lsl #48
cmp x10, x11
b.ne fail
adr x1, signed_data
.inst 0x3980002a
movz x11, #65408
movk x11, #65535, lsl #16
movk x11, #65535, lsl #32
movk x11, #65535, lsl #48
cmp x10, x11
b.ne fail
adr x1, signed_data
.inst 0x39c0002a
movz x11, #65408
movk x11, #65535, lsl #16
cmp x10, x11
b.ne fail
adr x1, signed_data
.inst 0x7980002a
movz x11, #65408
movk x11, #65535, lsl #16
movk x11, #65535, lsl #32
movk x11, #65535, lsl #48
cmp x10, x11
b.ne fail
adr x1, signed_data
.inst 0xb980002a
movz x11, #65408
movk x11, #65535, lsl #16
movk x11, #65535, lsl #32
movk x11, #65535, lsl #48
cmp x10, x11
b.ne fail
movz x1, #255
.inst 0x4e010c20
.inst 0x6f00e400
.inst 0x4e083c0a
movz x11, #0
cmp x10, x11
b.ne fail
adr x1, atomic_data
mov x2, #99
.inst 0x885ffc2a
.inst 0x8809fc22
cmp w9, #0
b.ne fail
ldr w10, [x1]
movz x11, #99
cmp x10, x11
b.ne fail
.inst 0x8809fc22
cmp w9, #1
b.ne fail
.inst 0xc85ffc2a
.inst 0xd5033f5f
.inst 0xc809fc22
cmp w9, #1
b.ne fail
mov x0, #0
b finish
fail:
mov x0, #42
finish:
mov x8, #93
svc #0
.data
.balign 16
signed_data:
.word 0xffffff80
.balign 16
atomic_data:
.quad 1
