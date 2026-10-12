.global _start
.text
.global restore_signal
restore_signal:
    mov x8, #139
    svc #0
.global check_signal_registers
check_signal_registers:
    stp x19, x30, [sp, #-16]!
    mov x19, #42
    movi v0.16b, #0x11
    mov x8, #172
    svc #0
    mov x1, #2
    mov x8, #129
    svc #0
    cmp x19, #99
    b.ne 1f
    fmov x0, d0
    b 2f
1:  mov x0, #0
2:  ldp x19, x30, [sp], #16
    ret
.global clobber_vector
clobber_vector:
    movi v0.16b, #0xff
    ret
_start:
    ldr x0, [sp]
    add x1, sp, #8
    add x2, x1, x0, lsl #3
    add x2, x2, #8
    bl guest_main
    movz x8, #93
    svc #0
