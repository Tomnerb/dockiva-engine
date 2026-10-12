/* ISA and memory checks assembled independently by LLVM. */
.global _start
.text
_start:
#if CASE == 1
    // 32-bit carry/zero without signed overflow.
    movn w1, #0
    adds w2, w1, #1
    b.ne fail
    b.cc fail
    b.vs fail
    // 32-bit signed overflow without carry.
    movz w1, #65535
    movk w1, #32767, lsl #16
    adds w2, w1, #1
    b.vc fail
    b.pl fail
    b.cs fail
    // 64-bit borrow.
    movz x1, #0
    subs x2, x1, #1
    b.cs fail
    b.eq fail
    b.pl fail
    b.vs fail
    // Negative minus positive overflow.
    movz x1, #32768, lsl #48
    subs x2, x1, #1
    b.vc fail
    b.mi fail
    b.cc fail
    // 32-bit MOVK clears upper bits, and MOVN is width-limited.
    movn x1, #0
    movk w1, #0, lsl #16
    movz x2, #65535
    cmp x1, x2
    b.ne fail
    // XZR writes must not mutate SP.
    mov x3, sp
    adds xzr, x2, #1
    mov x4, sp
    cmp x3, x4
    b.ne fail
#elif CASE == 2
    // SP aligned, pair pre/post-index, scalar widths, register offsets.
    mov x3, sp
    movz x1, #17
    movz x2, #34
    stp x1, x2, [sp, #-32]!
    ldrb w4, [sp]
    cmp w4, #17
    b.ne fail
    strh w2, [sp, #18]
    ldrh w4, [sp, #18]
    cmp w4, #34
    b.ne fail
    movz x4, #16
    str w1, [sp, x4]
    ldr w5, [sp, #16]
    cmp w5, #17
    b.ne fail
    ldp x4, x5, [sp], #32
    cmp x4, x1
    b.ne fail
    cmp x5, x2
    b.ne fail
    mov x4, sp
    cmp x3, x4
    b.ne fail
#elif CASE == 3
    // Read-only code store must fail in the interpreter.
    adr x1, _start
    str xzr, [x1]
#elif CASE == 4
    // Stack is RW, never executable.
    mov x1, sp
    br x1
#elif CASE == 5
    // Initial argc, argv terminator, empty env terminator, and auxv pagesize.
    ldr x0, [sp]
    cmp x0, #1
    b.ne fail
    ldr x1, [sp, #8]
    cbz x1, fail
    ldr x1, [sp, #16]
    cbnz x1, fail
    ldr x1, [sp, #24]
    cbnz x1, fail
    add x1, sp, #32
aux_loop:
    ldr x2, [x1]
    cbz x2, fail
    cmp x2, #6
    b.eq found_pagesize
    add x1, x1, #16
    b aux_loop
found_pagesize:
    ldr x2, [x1, #8]
    cmp x2, #1, lsl #12
    b.ne fail
#elif CASE == 6
    // Out-of-bounds load must fail.
    movz x1, #0
    ldr x0, [x1]
#elif CASE == 7
    // Misaligned SP base must fail even if its bytes are mapped.
    sub sp, sp, #8
    ldr x0, [sp]
#elif CASE == 8
    // Shifted arithmetic, signed shift, condition selection, TBZ/TBNZ, call/return.
    movz x1, #3
    add x2, x1, x1, lsl #2
    cmp x2, #15
    b.ne fail
    movn x3, #0
    add x4, x2, x3, asr #63
    cmp x4, #14
    b.ne fail
    cset w5, eq
    tbz w5, #0, fail
    tbnz w5, #1, fail
    csneg x6, x1, x2, ne
    movn x7, #14
    cmp x6, x7
    b.ne fail
    bl helper
    cmp x0, #77
    b.ne fail
    b success
helper:
    movz x0, #77
    ret
#endif
success:
    movz x0, #0
    movz x8, #93
    svc #0
fail:
    movz x0, #99
    movz x8, #93
    svc #0
