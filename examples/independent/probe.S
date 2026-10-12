// Independently assembled and linked Linux ARM64 syscall probes.
// CASE is supplied by the build script. No libc, stack or generated opcodes.
.section .rodata
message:
    .ascii "Independent ARM64 Linux probe\n"
message_end:

.global _start
.text
_start:
#if CASE == 1
    movz x0, #1
    adr x1, message
    movz x2, #30
    movz x8, #64
    svc #0
    movz x0, #0
#elif CASE == 2
    movz x0, #2
    adr x1, message
    movz x2, #30
    movz x8, #64
    svc #0
    movz x0, #42
#elif CASE == 3
    movz x0, #1
    adr x1, message
    movz x2, #30
    movz x8, #64
    svc #0
    // Preserve write's return value as the process exit code.
#elif CASE == 4
    movz x0, #1
    movz x1, #0
    movz x2, #0
    movz x8, #64
    svc #0
#elif CASE == 5
    movz x0, #65535
    adr x1, message
    movz x2, #1
    movz x8, #64
    svc #0
    // -EBADF (-9) becomes exit status 247.
#elif CASE == 6
    movz x0, #298
#else
#error Unknown CASE
#endif
#if CASE == 6
    movz x8, #94
#else
    movz x8, #93
#endif
    svc #0
