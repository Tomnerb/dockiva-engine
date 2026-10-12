// Reference Linux AArch64 assembly corresponding to build_hello.py.
// Optional independent build with an AArch64 Linux GNU toolchain:
// aarch64-linux-gnu-as -o hello.o hello.S
// aarch64-linux-gnu-ld -o hello-independent.elf hello.o
.global _start
.text
_start:
    movz x0, #1
    adr x1, message
    movz x2, #message_end-message
    movz x8, #64
    svc #0
    movz x0, #0
    movz x8, #93
    svc #0
message:
    .ascii "Hello from ARM64 Linux, without a Linux VM!\n"
message_end:
