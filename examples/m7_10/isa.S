.global _start
.text
.macro check reg, value
 movz x15, (\value & 65535)
 movk x15, ((\value >> 16) & 65535), lsl #16
 movk x15, ((\value >> 32) & 65535), lsl #32
 movk x15, ((\value >> 48) & 65535), lsl #48
 cmp \reg, x15
 b.ne failed
.endm
_start:
 mov x0, -1
 and x1, x0, #0xff00ff00ff00ff00
 check x1, 0xff00ff00ff00ff00
 mov w1, -1
 and w2, w1, #0x00ff00ff
 check x2, 0xff00ff
 ands xzr, x0, #1
 b.eq failed
 eor x2, x0, x0
 check x2, 0
 mov x1, 0x80
 sxtb x2, w1
 check x2, -128
 mov x1, 0xabcd
 ubfx x2, x1, #4, #8
 check x2, 0xbc
 mov x2, 0
 bfi x2, x1, #8, #8
 check x2, 0xcd00
 mov w1, -2
 mov x0, 9
 add x2, x0, w1, sxtw #2
 check x2, 1
 mov x1, 7
 mov x2, 6
 madd x3, x1, x2, x0
 check x3, 51
 mov x1, 0x8000000000000000
 mov x2, 2
 smulh x3, x1, x2
 check x3, -1
 umulh x3, x1, x2
 check x3, 1
 cmp x0, x0
 ccmp x1, #0, #0, eq
 b.eq failed
 ccmp x1, #0, #4, eq
 b.ne failed
 sub sp, sp, #64
 mov w1, 0x41
 dup v0.16b, w1
 stp q0, q0, [sp]
 ldp q1, q2, [sp]
 mov x2, v2.d[0]
 check x2, 0x4141414141414141
 str q1, [sp, #32]
 ldr q3, [sp, #32]
 mov x2, v3.d[0]
 check x2, 0x4141414141414141
 add sp, sp, #64
 mov x0, 0
 b done
failed:
 mov x0, 1
done:
 mov x8, 93
 svc #0
