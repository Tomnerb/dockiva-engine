.global _start
.text
_start:
    sub sp, sp, #64
    mov x1, sp
    mov x0, #0xffff
    stlrb w0, [x1]
    ldarb w2, [x1]
    cmp x2, #255
    b.ne fail
    stlrh w0, [x1]
    ldarh w2, [x1]
    cmp x2, x0
    b.ne fail
    mov x0, #-1
    stlr w0, [x1]
    ldar w2, [x1]
    lsr x3, x0, #32
    cmp x2, x3
    b.ne fail
    stlr x0, [x1]
    ldar x2, [x1]
    cmp x2, x0
    b.ne fail
    ldxr x4, [x1]
    stlr xzr, [x1]
    stxr w5, x4, [x1]
    // Store-exclusive may fail spuriously; either outcome is architectural.
    ldar xzr, [x1]
    .inst 0xd503241f
    .inst 0xd503245f
    .inst 0xd503249f
    .inst 0xd50324df
    movi v3.4s, #1
    str q3, [sp]
    ldr w0, [sp, #12]
    cmp w0, #1
    b.ne fail
    movi v3.2s, #255, lsl #24
    str q3, [sp]
    ldr w0, [sp]
    mov w2, #0xff000000
    cmp w0, w2
    b.ne fail
    ldr x0, [sp, #8]
    cbnz x0, fail
    fmov d3, #2.0
    str q3, [sp]
    ldr x0, [sp]
    mov x2, #0x4000000000000000
    cmp x0, x2
    b.ne fail
    fmov s3, #-1.5
    str q3, [sp]
    ldr w0, [sp]
    mov w2, #0xbfc00000
    cmp w0, w2
    b.ne fail
    adr x1, values
    ld1 {v3.16b}, [x1]
    mov x1, sp
    st1 {v3.16b}, [x1]
    ldr w0, [sp, #12]
    cmp w0, #4
    b.ne fail
    mov x1, sp
    mov x2, #16
    str q3, [x1, x2]
    ldr q4, [x1, x2]
    fmov d5, d4
    str d5, [sp]
    ldr w0, [sp]
    cmp w0, #1
    b.ne fail
    movi v4.4s, #0
    cmeq v4.16b, v4.16b, #0
    str q4, [sp]
    ldr x0, [sp]
    cmn x0, #1
    b.ne fail
    adr x1, values
    ld1 {v4.d}[1], [x1]
    add x1, sp, #16
    st1 {v4.d}[1], [x1]
    ldr w0, [sp, #16]
    cmp w0, #1
    b.ne fail
    movi v6.16b, #255
    bit v6.16b, v3.16b, v6.16b
    str q6, [sp]
    ldr w0, [sp]
    cmp w0, #1
    b.ne fail
    movi v7.16b, #255
    movi v8.16b, #127
    cmhs v9.16b, v7.16b, v8.16b
    str q9, [sp]
    ldr x0, [sp]
    cmn x0, #1
    b.ne fail
    cmhi v9.2d, v7.2d, v7.2d
    str q9, [sp]
    ldr x0, [sp]
    cbnz x0, fail
    cmhs v9.4s, v7.4s, v7.4s
    str q9, [sp]
    ldr x0, [sp]
    cmn x0, #1
    b.ne fail
    adr x1, values
    mov x2, x1
    ld1 {v10.16b}, [x1], #16
    sub x0, x1, x2
    cmp x0, #16
    b.ne fail
    mov x1, sp
    st1 {v10.16b}, [x1], #16
    mov x2, sp
    sub x0, x1, x2
    cmp x0, #16
    b.ne fail
    ldr w0, [sp, #12]
    cmp w0, #4
    b.ne fail
    // Narrowing truncates, uses original aliased source, and SHRN2 preserves low half.
    movi v7.16b, #255
    shrn v7.8b, v7.8h, #4
    str q7, [sp]
    ldr x0, [sp]
    cmn x0, #1
    b.ne fail
    ldr x0, [sp, #8]
    cbnz x0, fail
    movi v8.16b, #255
    shrn2 v7.16b, v8.8h, #8
    str q7, [sp]
    ldr x0, [sp, #8]
    cmn x0, #1
    b.ne fail
    shrn v7.4h, v8.4s, #16
    shrn2 v7.8h, v8.4s, #1
    str q7, [sp]
    ldr x0, [sp, #8]
    cmn x0, #1
    b.ne fail
    shrn v7.2s, v8.2d, #32
    shrn2 v7.4s, v8.2d, #1
    str q7, [sp]
    ldr x0, [sp, #8]
    cmn x0, #1
    b.ne fail
    movi v8.16b, #255
    addhn v7.8b, v8.8h, v8.8h
    addhn2 v7.16b, v8.8h, v8.8h
    str q7, [sp]
    ldr x0, [sp]
    cmn x0, #1
    b.ne fail
    ldr x0, [sp, #8]
    cmn x0, #1
    b.ne fail
    addhn v7.4h, v8.4s, v8.4s
    addhn2 v7.8h, v8.4s, v8.4s
    str q7, [sp]
    ldr x0, [sp, #8]
    cmn x0, #1
    b.ne fail
    addhn v7.2s, v8.2d, v8.2d
    addhn2 v7.4s, v8.2d, v8.2d
    str q7, [sp]
    ldr x0, [sp, #8]
    cmn x0, #1
    b.ne fail
    // Scalar SIMD register offsets: scaled positive and signed negative indexes.
    mov x1, sp
    mov x2, #2
    str s7, [x1, x2, lsl #2]
    ldr s9, [x1, w2, uxtw #2]
    str q9, [sp, #16]
    ldr w0, [sp, #16]
    cmn w0, #1
    b.ne fail
    ldr x0, [sp, #24]
    cbnz x0, fail
    add x1, sp, #16
    mov w2, #-1
    str d7, [x1, w2, sxtw #3]
    ldr d9, [x1, w2, sxtw #3]
    str d9, [sp]
    ldr x0, [sp]
    cmn x0, #1
    b.ne fail
    movi v4.16b, #255
    bic v4.8h, #0xf0
    str q4, [sp]
    ldrh w0, [sp]
    mov w2, #0xff0f
    cmp w0, w2
    b.ne fail
    orr v4.4h, #0x80, lsl #8
    str q4, [sp]
    ldr x0, [sp, #8]
    cbnz x0, fail
    bic v4.2s, #255, lsl #24
    orr v4.2s, #0x40, lsl #24
    str q4, [sp]
    ldr w0, [sp]
    mov w2, #0xff0f
    movk w2, #0x400f, lsl #16
    cmp w0, w2
    b.ne fail
    mov x1, sp
    mov w2, #255
    strb w2, [x1]
1:  ldaxrb w0, [x1]
    cmp w0, #255
    b.ne fail
    stlxrb w3, w2, [x1]
    cbnz w3, 1b
    mov w2, #65535
    strh w2, [x1]
2:  ldaxrh w0, [x1]
    cmp w0, w2
    b.ne fail
    stlxrh w3, w2, [x1]
    cbnz w3, 2b
    movi v4.8h, #8
    str q4, [sp]
    ldr w0, [sp]
    mov w2, #8
    movk w2, #8, lsl #16
    cmp w0, w2
    b.ne fail
    movi v4.4h, #7, lsl #8
    str q4, [sp]
    ldr w0, [sp]
    mov w2, #0x700
    movk w2, #0x700, lsl #16
    cmp w0, w2
    b.ne fail
    ldr x0, [sp, #8]
    cbnz x0, fail
    mov x16, #1
    hint #40
    cmp x16, #1
    b.ne fail
    paciasp
    autiasp
    pacibsp
    autibsp
    adr x1, values
    ldp q0, q1, [x1]
    addp v2.4s, v0.4s, v1.4s
    str q2, [sp]
    ldr w0, [sp]
    cmp w0, #3
    b.ne fail
    ldr w0, [sp, #12]
    cmp w0, #15
    b.ne fail
    uzp1 v2.4s, v0.4s, v1.4s
    str q2, [sp]
    ldr w0, [sp]
    cmp w0, #1
    b.ne fail
    ldr w0, [sp, #4]
    cmp w0, #3
    b.ne fail
    ldr w0, [sp, #8]
    cmp w0, #5
    b.ne fail
    ldr w0, [sp, #12]
    cmp w0, #7
    b.ne fail
    uzp2 v2.4s, v0.4s, v1.4s
    str q2, [sp]
    ldr w0, [sp]
    cmp w0, #2
    b.ne fail
    ldr w0, [sp, #12]
    cmp w0, #8
    b.ne fail
    movi v0.16b, #255
    umull v0.8h, v0.8b, v0.8b
    str q0, [sp]
    ldrh w0, [sp, #14]
    mov w1, #65025
    cmp w0, w1
    b.ne fail
    movi v0.16b, #255
    umull v0.4s, v0.4h, v0.4h
    str q0, [sp]
    ldr w0, [sp, #12]
    mov w1, #1
    movk w1, #0xfffe, lsl #16
    cmp w0, w1
    b.ne fail
    movi v0.16b, #255
    umull v0.2d, v0.2s, v0.2s
    str q0, [sp]
    ldr x0, [sp, #8]
    mov x1, #0xfffffffe00000001
    cmp x0, x1
    b.ne fail
    adr x1, values
    ldr q0, [x1]
    umull2 v0.2d, v0.4s, v0.4s
    str q0, [sp]
    ldr x0, [sp]
    cmp x0, #9
    b.ne fail
    ldr x0, [sp, #8]
    cmp x0, #16
    b.ne fail
    movi v0.16b, #255
    movi v1.16b, #1
    uaddw v0.8h, v0.8h, v1.8b
    str q0, [sp]
    ldrh w0, [sp]
    cbnz w0, fail
    movi v0.16b, #255
    uaddw v0.4s, v0.4s, v1.4h
    str q0, [sp]
    ldr w0, [sp]
    cmp w0, #256
    b.ne fail
    movi v0.16b, #255
    adr x1, values
    ldr q1, [x1]
    uaddw2 v0.2d, v0.2d, v1.4s
    str q0, [sp]
    ldr x0, [sp]
    cmp x0, #2
    b.ne fail
    ldr x0, [sp, #8]
    cmp x0, #3
    b.ne fail
    adr x1, values
    ldr q0, [x1]
    addp d0, v0.2d
    str q0, [sp]
    ldr x0, [sp]
    mov x1, #4
    movk x1, #6, lsl #32
    cmp x0, x1
    b.ne fail
    ldr x0, [sp, #8]
    cbnz x0, fail
    mov x1, #0x8000000000000001
    fmov d0, x1
    fmov x2, d0
    cmp x1, x2
    b.ne fail
    str q0, [sp]
    ldr x0, [sp, #8]
    cbnz x0, fail
    fmov d0, xzr
    fmov x2, d0
    cbnz x2, fail
    mov x1, #0x0100
    movk x1, #0x0703, lsl #16
    movk x1, #0x1f0f, lsl #32
    movk x1, #0xff7f, lsl #48
    fmov d0, x1
    cnt v0.8b, v0.8b
    fmov x0, d0
    mov x1, #0x0100
    movk x1, #0x0302, lsl #16
    movk x1, #0x0504, lsl #32
    movk x1, #0x0807, lsl #48
    cmp x0, x1
    b.ne fail
    str q0, [sp]
    ldr x0, [sp, #8]
    cbnz x0, fail
    movi v0.16b, #255
    cnt v0.16b, v0.16b
    str q0, [sp]
    ldrb w0, [sp, #15]
    cmp w0, #8
    b.ne fail
    movi v0.16b, #255
    addv b0, v0.8b
    str q0, [sp]
    ldr x0, [sp]
    cmp x0, #248
    b.ne fail
    movi v0.16b, #255
    addv h0, v0.8h
    str q0, [sp]
    ldr x0, [sp]
    mov x1, #65528
    cmp x0, x1
    b.ne fail
    adr x1, values
    ldr q0, [x1]
    addv s0, v0.4s
    str q0, [sp]
    ldr x0, [sp]
    cmp x0, #10
    b.ne fail
    ldr x0, [sp, #8]
    cbnz x0, fail
    adr x1, values
    ldp q0, q1, [x1]
    ext v0.16b, v0.16b, v1.16b, #8
    str q0, [sp]
    ldr w0, [sp]
    cmp w0, #3
    b.ne fail
    ldr w0, [sp, #12]
    cmp w0, #6
    b.ne fail
    ldp q0, q1, [x1]
    ext v1.8b, v0.8b, v1.8b, #4
    str q1, [sp]
    ldr w0, [sp]
    cmp w0, #2
    b.ne fail
    ldr w0, [sp, #4]
    cmp w0, #5
    b.ne fail
    ldr x0, [sp, #8]
    cbnz x0, fail
    movi v0.16b, #255
    mov x0, #0x1234
    ins v0.d[1], x0
    str q0, [sp]
    ldr x0, [sp]
    cmn x0, #1
    b.ne fail
    ldr x0, [sp, #8]
    mov x1, #0x1234
    cmp x0, x1
    b.ne fail
    ins v0.b[3], wzr
    str q0, [sp]
    ldrb w0, [sp, #3]
    cbnz w0, fail
    ldrb w0, [sp, #2]
    cmp w0, #255
    b.ne fail
    mov x0, #0xcdef
    ins v0.h[7], w0
    ins v0.s[1], w0
    str q0, [sp]
    ldrh w0, [sp, #14]
    mov w1, #0xcdef
    cmp w0, w1
    b.ne fail
    ldr w0, [sp, #4]
    cmp w0, w1
    b.ne fail
    mov x1, #-1
    cmp xzr, xzr
    adcs x0, x1, xzr
    b.cc fail
    b.ne fail
    cbnz x0, fail
    mov w1, #0
    mov w2, #1
    cmp wzr, w2
    sbcs w0, w1, wzr
    b.cs fail
    b.pl fail
    mov x1, #0xffffffff
    cmp x0, x1
    b.ne fail
    mov x1, #0x8000000000000000
    mov x2, #1
    cmp xzr, xzr
    sbcs x0, x1, x2
    b.vc fail
    b.cc fail
    mov x1, #0x7fffffffffffffff
    cmp x0, x1
    b.ne fail
    movi v0.16b, #255
    mov w1, #0xbf800000
    fmov s0, w1
    mov x2, #-1
    fmov w2, s0
    cmp x1, x2
    b.ne fail
    str q0, [sp]
    ldr w0, [sp, #4]
    cbnz w0, fail
    ldr x0, [sp, #8]
    cbnz x0, fail
    adr x1, values
    ldr q0, [x1]
    ins v0.d[1], v0.d[0]
    str q0, [sp]
    ldr x0, [sp]
    ldr x1, [sp, #8]
    cmp x0, x1
    b.ne fail
    ins v0.b[15], v0.b[0]
    str q0, [sp]
    ldrb w0, [sp, #15]
    cmp w0, #1
    b.ne fail
    ins v0.h[3], v0.h[0]
    ins v0.s[3], v0.s[0]
    str q0, [sp]
    ldrh w0, [sp, #6]
    cmp w0, #1
    b.ne fail
    ldr w0, [sp, #12]
    cmp w0, #1
    b.ne fail
    // Four-way de-interleave, register wrap, 64-bit vector upper clearing.
    adr x1, structures
    ld4 {v30.8b, v31.8b, v0.8b, v1.8b}, [x1], #32
    str q0, [sp]
    ldr x0, [sp]
    mov x2, #0x0303
    movk x2, #0x0303, lsl #16
    movk x2, #0x0303, lsl #32
    movk x2, #0x0303, lsl #48
    cmp x0, x2
    b.ne fail
    ldr x0, [sp, #8]
    cbnz x0, fail
    adr x2, structures
    add x2, x2, #32
    cmp x1, x2
    b.ne fail
    mov x1, sp
    st4 {v30.8b, v31.8b, v0.8b, v1.8b}, [x1]
    ldr w0, [sp, #28]
    mov w2, #0x0201
    movk w2, #0x0403, lsl #16
    cmp w0, w2
    b.ne fail
    // LD1 multiple registers is sequential, unlike LD4.
    adr x1, structures
    mov x3, #7
    ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x1], x3
    adr x2, structures
    add x2, x2, #7
    cmp x1, x2
    b.ne fail
    str q7, [sp]
    ldr w0, [sp]
    mov w2, #0x0605
    movk w2, #0x0807, lsl #16
    cmp w0, w2
    b.ne fail
    mov x0, #0x1234
    mov x1, #0xabcd
    extr x2, x0, x1, #4
    mov x3, #0x0abc
    movk x3, #0x4000, lsl #48
    cmp x2, x3
    b.ne fail
    extr w2, w0, w1, #0
    cmp w2, w1
    b.ne fail
    movi v0.16b, #255
    sshll v1.8h, v0.8b, #0
    str q1, [sp]
    ldr x0, [sp]
    cmn x0, #1
    b.ne fail
    ushll2 v1.8h, v0.16b, #7
    str q1, [sp]
    ldrh w0, [sp, #14]
    mov w1, #32640
    cmp w0, w1
    b.ne fail
    shll2 v1.8h, v0.16b, #8
    ushr v2.8h, v1.8h, #8
    sshr v3.8h, v1.8h, #16
    shl v2.8h, v2.8h, #8
    str q2, [sp]
    ldrh w0, [sp, #14]
    mov w1, #65280
    cmp w0, w1
    b.ne fail
    mov h3, v3.h[7]
    str q3, [sp]
    ldr x0, [sp]
    mov x1, #65535
    cmp x0, x1
    b.ne fail
    ldr x0, [sp, #8]
    cbnz x0, fail
    // Signed pair loads, non-temporal pairs, reductions and lane moves.
    mov w0, #-2
    mov w1, #7
    stp w0, w1, [sp]
    mov x2, sp
    ldpsw x0, x1, [x2], #8
    cmn x0, #2
    b.ne fail
    cmp x1, #7
    b.ne fail
    sub x2, x2, #8
    stnp x0, x1, [x2]
    ldnp x3, x4, [x2]
    cmp x3, x0
    b.ne fail
    cmp x4, x1
    b.ne fail
    mov x5, sp
    cmp x2, x5
    b.ne fail
    movi v0.16b, #9
    movi v1.16b, #3
    uminp v2.16b, v0.16b, v1.16b
    umov w0, v2.b[7]
    cmp w0, #9
    b.ne fail
    umov w0, v2.b[8]
    cmp w0, #3
    b.ne fail
    movi d0, #0xff00ff00ff00ff00
    umov x0, v0.d[0]
    mov x1, #0xff00
    movk x1, #0xff00, lsl #16
    movk x1, #0xff00, lsl #32
    movk x1, #0xff00, lsl #48
    cmp x0, x1
    b.ne fail
    umov x0, v0.d[1]
    cbnz x0, fail
    mvni v1.2s, #0
    umov w0, v1.s[1]
    cmn w0, #1
    b.ne fail
    mov x1, #-1
    prfm pldl1keep, [x1]
    prfum pldl1keep, [x1, #-1]
    prfm pldl1keep, [x1, xzr]
    mov x0, #0
    b exit
fail:
    mov x0, #1
exit:
    mov x8, #93
    svc #0

.balign 16
values: .word 1,2,3,4,5,6,7,8

.balign 16
structures:
.rept 8
.byte 1,2,3,4
.endr
.rept 8
.byte 5,6,7,8
.endr
