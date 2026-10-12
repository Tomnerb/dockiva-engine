.global _start
.text
_start:
    ldr x0, [sp]
    cmp x0, #2
    b.eq exec_child
    // A pipe proves that the parent resumes at exec, before child exit.
    adr x0, pipes
    mov x1, #0
    mov x8, #59
    svc #0
    cbnz x0, fail
    adr x0, pipes
    ldr w0, [x0]
    mov x1, #0
    mov x2, #0
    mov x8, #24
    svc #0
    cbnz x0, fail
    mov x0, #16657
    adr x1, child_stack_end
    mov x2, #0
    mov x3, #0
    mov x4, #0
    mov x8, #220
    svc #0
    cbz x0, child
    cmp x0, #0
    b.lt fail
    mov x19, x0
    adr x1, shared_value
    ldr x2, [x1]
    cmp x2, #42
    b.ne fail
    adr x2, pipes
    ldr w0, [x2, #4]
    mov x2, #1
    mov x8, #64
    svc #0
    cmp x0, #1
    b.ne fail
    mov x0, x19
    adr x1, status
    mov x2, #0
    mov x3, #0
    mov x8, #260
    svc #0
    cmp x0, x19
    b.ne fail
    adr x1, status
    ldr w0, [x1]
    cmp w0, #1792
    b.ne fail
    // Reuse the surviving address space for a second vfork that exits.
    mov x0, #16657
    adr x1, child_stack_end
    mov x8, #220
    svc #0
    cbz x0, exit_child
    cmp x0, #0
    b.lt fail
    mov x19, x0
    adr x1, shared_value
    ldr x2, [x1]
    cmp x2, #84
    b.ne fail
    adr x1, status
    mov x2, #0
    mov x3, #0
    mov x8, #260
    svc #0
    cmp x0, x19
    b.ne fail
    adr x1, status
    ldr w0, [x1]
    cmp w0, #2304
    b.ne fail
    mov x0, #0
    b exit
exit_child:
    adr x1, shared_value
    mov x0, #84
    str x0, [x1]
    mov x0, #9
    b exit
child:
    // Delay beyond one scheduler slice; the parent must remain suspended.
    mov x0, #10000
1:  subs x0, x0, #1
    b.ne 1b
    adr x1, shared_value
    mov x0, #42
    str x0, [x1]
    adr x0, bad_executable
    adr x1, arguments
    mov x2, #0
    mov x8, #221
    svc #0
    cmn x0, #8
    b.ne fail
    adr x1, shared_value
    ldr x0, [x1]
    cmp x0, #42
    b.ne fail
    adr x0, executable
    adr x1, arguments
    mov x2, #0
    mov x8, #221
    svc #0
    b fail
exec_child:
    mov x0, #0
    adr x1, shared_value
    mov x2, #1
    mov x8, #63
    svc #0
    cmp x0, #1
    b.ne fail
    adr x1, shared_value
    ldrb w0, [x1]
    cmp w0, #42
    b.ne fail
    mov x0, #7
    b exit
fail:
    mov x0, #1
exit:
    mov x8, #93
    svc #0
.data
.balign 16
shared_value: .quad 0
pipes: .word 0, 0
status: .word 0
bad_executable: .asciz "/bad.elf"
executable: .asciz "/vfork.elf"
argument: .asciz "child"
.balign 8
arguments: .quad executable, argument, 0
.balign 16
child_stack: .skip 1024
child_stack_end:
