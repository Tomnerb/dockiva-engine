# Milestone 3: freestanding C program and Linux startup stack

The C backend now runs a small independently compiled ARM64 Linux C program
that reads arguments, uses global writable memory and local stack memory,
computes a digit sum, prints the result, reads its environment, and returns an
input-dependent exit status. Guest behavior is executed from its ELF instructions.

## Mac acceptance

Extract the complete package into a fresh directory. Python 3.10+ and a local C
compiler (Xcode Command Line Tools) are required. Zig is only needed to rebuild
Linux fixtures, not to run this release.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m3.py --report mac-m3-validation.json
python3 scripts/verify_m2.py
python3 -m unittest discover -s tests -v
```

Expect 22 PASS lines from M3, 20 PASS lines from M2, and the original 13 Python
unittest methods passing. `Linux reference: not-run` is expected in the Mac check.

Manual example:

```sh
./build/vmfree-c run examples/m3/demo.elf --env MODE=demo -- 12345
```

Expected stdout:

```text
sum=15
MODE=demo
```

This program intentionally exits with status 15 (the computed sum). The verifier
knows the expected status and itself exits zero when all checks pass.

The program accepts one string containing 1 to 9 ASCII digits. `999999999` prints
`sum=81` and returns 81. Invalid or empty input returns 3. Wrong argument count
prints usage and returns 2. The first supplied environment entry is printed,
truncated to 128 bytes; no environment output is produced when none is supplied.

## New execution support

- Writable PT_LOAD data, zero-filled BSS and a non-executable 1 MiB guest stack.
- MOVN/MOVZ/MOVK and MOV-register alias; ADR and ADRP.
- ADD/SUB immediate and shifted register, with optional NZCV flag updates.
- Conditional branches, CBZ/CBNZ, TBZ/TBNZ, BL, BR, BLR and RET.
- CSEL/CSINC/CSINV/CSNEG, including the CSET alias used by the compiler.
- Unsigned scalar byte/halfword/word/doubleword loads and stores: unsigned
  immediate, unscaled, pre/post-index and supported register-offset forms.
- 32/64-bit load/store pairs in offset and pre/post-index forms.
- SP versus XZR handling, W-register zero extension, guest permission/bounds
  checks, and rejection of supported-family reserved/unpredictable encodings.

This is a subset of the ARM64 ISA. Signed loads, vector/SIMD, floating point,
atomics and many other forms remain unsupported. The diagnostic reports the
instruction and PC when an unknown opcode is reached. Architectural corner cases
beyond the exercised subset are not claimed conformant.

## Initial stack contract

The runtime sets SP to a 16-byte-aligned table:

`argc, argv pointers, NULL, envp pointers, NULL, auxiliary-vector pairs, AT_NULL`

Guest argv[0] is the ELF path passed to the runtime. Strings are NUL-terminated.
Use `--` to begin guest arguments, and repeat `--env KEY=VALUE` before `--` to
supply environment entries. The host environment is not implicitly copied into
the guest. At most 63 extra arguments, 64 environment entries, and 64 KiB of
combined argument/environment string storage are accepted.

The fixed guest stack range starts at 0x7fffffe00000 and occupies 1 MiB. ELF load
segments overlapping it are rejected. Stack memory is separate from the existing
16 MiB ELF segment cap. Initial auxiliary entries include AT_PHDR when the header
is mapped, AT_PHENT, AT_PHNUM, AT_PAGESZ=4096, AT_BASE=0, AT_ENTRY and AT_NULL.
This is a minimal static-executable startup contract, not a complete Linux auxv.
There is no AT_RANDOM, vDSO, TLS setup, ASLR or libc startup support.

## Validation and independent reference

The 22 M3 cases comprise:

- 12 compiled-C input/environment cases, including non-ASCII rejection, maximum
  sum, empty input, extra arguments, and environment truncation.
- Four independent assembly checks covering arithmetic flags and widths,
  memory/pairs, startup layout/auxv, and branches/selection/calls.
- Six interpreter rejection checks: read-only writes, stack execution, unmapped
  reads, misaligned SP, excessive argument count, and excessive startup bytes.

The 16 non-fault cases were compared byte-for-byte and by exit code against QEMU
7.2.22 user-mode on Linux using the exact same ELF binaries. The six rejection
cases exercise runtime policy; they are not Linux crash-behavior comparisons.
M3 Mac execution and native ARM64 Linux hardware checks remain pending.

To repeat the Linux reference on x86 Linux:

```sh
python3 scripts/verify_m3.py --qemu /path/to/qemu-aarch64 --report linux-m3.json
```

On ARM64 Linux hardware:

```sh
chmod +x examples/m3/*.elf
python3 scripts/verify_m3.py --native-linux --report arm64-m3.json
```

Native-reference subprocess environments are deliberately limited to each case's
explicit test variables. QEMU is only a validation dependency, never a runtime
backend or a Mac dependency.

## Rebuild guest fixtures

```sh
python3 scripts/build_m3.py --zig /absolute/path/to/zig
```

Tested with Zig 0.13.0/LLVM. The script records source hashes, ELF hashes and flags.
The C demo is compiled freestanding at O0, without libc, compiler runtime checks,
vectorization or stack-protector helpers. An assembly entry stub reads the initial
stack and calls the ordinary C function. This deliberately constrained build does
not establish compatibility with arbitrary C compiler output or Linux applications.
The supplied ELF files were independently built by LLVM, not our Python ELF builder.

## Reference scope and next work

Python remains the M0/M1 instruction and syscall reference. It has not acquired
M3 instructions or startup data. M3 conformance uses independent Linux reference
execution, while M2 verifies the shared subset against Python.

Heap allocation (brk/mmap), libc, dynamic linking, threads, signals and networking
are still unimplemented. This is not a container sandbox or a Docker engine.
A suitable next milestone is bounded guest heap mappings and additional static C
programs before introducing sockets and a server. The original roadmap's wider
M3 heap work and M2 JIT/VM comparisons remain open rather than silently completed.

Primary references used for the implementation:
- [ARM AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)
- [Linux ELF startup implementation](https://github.com/torvalds/linux/blob/master/fs/binfmt_elf.c)
