.global _start
.text
_start:
    adr x20, output
    mov x21, x20
    adr x22, values
    mov x0, #0x1234
    dup v0.2d, x0
    str q0, [x20], #16
    dup v1.4s, w0
    str q1, [x20], #16
    movi v2.16b, #2
    movi v3.4s, #0
    ldr q4, [x22]
    zip1 v5.16b, v4.16b, v2.16b
    str q5, [x20], #16
    zip2 v5.8h, v4.8h, v3.8h
    str q5, [x20], #16
    tbl v5.16b, {v4.16b}, v4.16b
    str q5, [x20], #16
    add v5.2d, v0.2d, v0.2d
    str q5, [x20], #16
    sub v5.4s, v3.4s, v1.4s
    str q5, [x20], #16
    cmeq v5.4s, v1.4s, v1.4s
    str q5, [x20], #16
    orr v5.16b, v4.16b, v2.16b
    str q5, [x20], #16
    umaxp v5.4s, v4.4s, v1.4s
    str q5, [x20], #16
    fmov x0, d5
    str x0, [x20], #8
    mvni v6.2s, #0
    str q6, [x20], #16
    stp d0, d1, [x20]
    ldp d7, d8, [x20]
    str q7, [x20], #16
    str q8, [x20], #16
    str b2, [x20]
    ldr b9, [x20]
    str q9, [x20], #16
    str h1, [x20]
    ldr h9, [x20]
    str q9, [x20], #16
    str s1, [x20]
    ldr s9, [x20]
    str q9, [x20], #16
    dmb ish
    dsb sy
    isb
    mov x0, #1
    mov x1, x21
    sub x2, x20, x21
    mov x8, #64
    svc #0
    mov x0, #0
    mov x8, #93
    svc #0
.data
.balign 16
values:
    .byte 0,1,2,3,4,5,6,7,8,9,10,11,12,13,16,255
output:
    .zero 512
