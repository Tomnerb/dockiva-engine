# Milestone notes

Scope, commands and results of each milestone, oldest first, merged from the former
`docs/MILESTONE-*.md` and `docs/M14-*.md` files. The live status ledger is
[MILESTONE-STATUS.md](MILESTONE-STATUS.md). Reports named `mac-*.json` are kept in
[evidence/mac-validation.json](../evidence/mac-validation.json), keyed by that file name;
the `--report` commands below write a fresh copy at the repository root.

- [Milestone 1: independently built Linux ELF probes](#m1)
- [Milestone 2: compiled interpreter experiment](#m2)
- [Milestone 3: freestanding C program and Linux startup stack](#m3)
- [Milestone 4: bounded guest allocation](#m4)
- [Milestone 5: a Linux HTTP server over real TCP](#m5)
- [Milestone 6: local OCI image execution](#m6)
- [Combined Milestones 7–10](#m7-10)
- [Combined M11–M17 release candidate](#m11-17)
- [M13 security review and open acceptance gate](#security-review-m13)
- [M17 macOS distribution tooling](#mac-distribution-m17)
- [macOS build fix 1](#mac-build-fix-1)
- [M14 performance checkpoint](#m14-benchmark)
- [M14 published-port fix](#m14-bind-fix)
- [M14 bounded instruction-classification cache](#m14-decode-cache)
- [M14 synchronized host-process measurement](#m14-host-load)
- [M14 immutable-segment lookup cache](#m14-lookup-cache)
- [M14 matched-rate and counter profiling update](#m14-matched-profile)
- [M14 sustained comparison](#m14-sustained)
- [M18: static BusyBox command compatibility](#m18)
- [M19: bounded guest processes and a basic noninteractive shell](#m19)
- [M20: bounded pipes and descriptor redirection](#m20)
- [M21: concurrent pipeline streaming and backpressure](#m21)
- [M22 — scheduled guest cancellation](#m22)
- [M23 — foreground input and guest process groups](#m23)
- [M24 — bounded scripts with unmodified upstream ash](#m24)
- [M25 — bounded ash job control and caught signals](#m25)
- [M26: scheduled networking foundation](#m26)
- [M27: filesystem compatibility (in progress)](#m27)
- [M28: bounded ARM64 musl dynamic loading](#m28)
- [M29: first practical application candidate](#m29)
- [M30: differential compatibility harness](#m30)
- [M31: confinement and aggregate limits (in progress)](#m31)
- [M32: representative performance feasibility (in progress)](#m32)
- [M35: instruction compatibility work in progress](#m35)
- [M39: glibc startup work in progress](#m39)
- [M44: filesystem prerequisites in progress](#m44)

<a id="m1"></a>

## Milestone 1: independently built Linux ELF probes

The six supplied programs are assembled and linked by Zig 0.13.0's LLVM toolchain.
They do not use `examples/build_hello.py` or runtime code when building the ELF.
They are small assembly programs, not general C/Go applications. This milestone
tests loader and syscall behavior; it does not expand the runtime's instruction set.

### Mac check (no compiler or Docker needed)

Extract this complete package into a fresh directory, then:

```sh
cd vmfree-runtime
python3 scripts/verify_m1.py --report mac-m1-report.json
python3 -m unittest discover -s tests -v
```

Expect six PASS lines. `Linux reference: not-run` is expected on this command;
it does not pretend to perform Linux reference execution on your Mac.
`mac-m1-report.json` records the host, ELF hashes, exact bytes and exit codes.

### What each probe verifies

| Probe | Expected stdout | Expected stderr | Exit |
|---|---|---|---:|
| stdout | Independent ARM64 Linux probe + newline | Empty | 0 |
| stderr_exit42 | Empty | Independent ARM64 Linux probe + newline | 42 |
| write_return | Independent ARM64 Linux probe + newline | Empty | 30 |
| zero_write | Empty | Empty | 0 |
| bad_fd | Empty | Empty | 247 (-EBADF truncated to 8 bits) |
| exit_group | Empty | Empty | 42 (298 truncated to 8 bits) |

The build manifest binds binaries to source using SHA-256. These hashes detect
accidental changes; they are not a signature or a guarantee of authenticity.

### Rebuild independently

With Zig 0.13.0 available on the host (Mac or Linux):

```sh
python3 scripts/build_independent.py --zig /absolute/path/to/zig
python3 scripts/verify_m1.py
```

The script uses `zig cc -target aarch64-linux-musl -nostdlib -static` and writes
the exact flags, compiler version and hashes into `build-manifest.json`.
No Linux VM is needed for cross-compilation. Do not regenerate the M0 fixture to
validate these probes; they use a separate build path.

### Reference on ARM64 Linux hardware

Copy the same extracted package to an ARM64 Linux machine:

```sh
chmod +x examples/independent/*.elf
python3 scripts/verify_m1.py --native-linux --report arm64-linux-m1-report.json
```

The script compares stdout, stderr and status from our interpreter with direct
execution of the exact same binary on that Linux machine. The platform guard
rejects this option on macOS and x86 Linux. Hardware execution remains a distinct
acceptance gate even if an emulated reference passes.

### Optional reference on x86 Linux

If qemu-aarch64 user-mode is installed:

```sh
python3 scripts/verify_m1.py --qemu /usr/bin/qemu-aarch64 --report qemu-m1-report.json
```

This reference uses QEMU instruction translation and the host Linux kernel.
It is labeled `qemu-user-linux`, not native ARM64 hardware validation. QEMU is only
a test oracle here; it is not invoked by the runtime and is not a Mac dependency.

### Remaining scope

M1 Mac acceptance was subsequently reported successful by the user (all six
probes and the test suite). Native ARM64 Linux hardware comparison remains pending.
There is no Docker image support, process isolation or performance claim.

References:
- [Zig toolchain](https://ziglang.org/)
- [QEMU user-mode documentation](https://www.qemu.org/docs/master/user/main.html)

<a id="m2"></a>

## Milestone 2: compiled interpreter experiment

This package adds a portable C11 implementation of the existing interpreter.
The host executable is native code; guest ARM64 instructions are still interpreted.
No JIT, Linux VM, or Docker daemon is involved. C is the experiment's language,
not a final commitment for the production execution core.

### Run on your Mac

Extract into a fresh directory. Requires Python 3.10+ and a local C compiler
(Xcode Command Line Tools provides `cc`). The package contains source, not a
prebuilt or signed Mac executable.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m2.py --report mac-m2-validation.json
python3 scripts/benchmark_m2.py --report mac-m2-benchmark.json
```

Expect 20 PASS lines from verification and two benchmark summary lines. If your
Mac reports that developer tools are missing, install Xcode Command Line Tools
and rerun the build. The build uses your Mac's default architecture.

To run one program manually:

```sh
./build/dockiva-runtime run examples/independent/stdout.elf --trace
```

The original Python tests remain available:

```sh
python3 -m unittest discover -s tests -v
```

### Verification

The verifier first checks source/binary hashes and the Python M1 results. It then
compares the C backend against all six expected stdout/stderr/status triples.
Fourteen additional cases compare bounded rejection with the Python reference:
truncation, wrong architecture, overflowed program-header offset, excessive guest
memory, nonexecutable entry, invalid instruction, unsupported syscall, infinite
branches and reserved instruction encoding. Diagnostic wording may differ.

The ELF loader decodes bytes explicitly rather than casting untrusted bytes to
host structures. Bounds are checked with subtraction to avoid unsigned overflow.
Guest memory remains interpreter-owned and never becomes a host execution pointer.

This is still not an untrusted-code sandbox. System-call coverage, host-error
translation, instruction support and resource limits remain deliberately small.

### Benchmarks and interpretation

- `startup_exit`: launch runtime, load ELF, interpret exit_group, terminate.
- `branch_dispatch`: launch runtime, execute 500,000 branch instructions, hit the
  instruction budget and terminate with the expected error. This is a synthetic
  dispatch test, not a complete application or an arithmetic workload.
- One warmup per backend, seven measured samples, alternating backend order.
- Median wall time includes process creation, ELF loading and teardown.
- CPU time and peak RSS come from OS child-process accounting in a fresh worker.
  Peak RSS can include inherited process memory. Do not treat it as steady-state
  engine memory. In this Linux environment both engines reported the same RSS
  floor, so these results do not establish a memory advantage.
- Raw samples, host, compiler options and binary hash are saved in JSON.
- `--samples 15 --steps 1000000` can increase the sample count and dispatch work.

Do not extrapolate Linux timings to macOS or ratios to real applications.
No Docker/lightweight-VM baseline was measured. No JIT or direct guest execution
was implemented. Those remain separate parts of the broader backend research.

### Sanitizers (optional developer check)

```sh
python3 scripts/build_native.py --sanitize
python3 scripts/verify_m2.py --native build/dockiva-runtime-sanitize
```

The Linux validation here used AddressSanitizer and UndefinedBehaviorSanitizer.
LeakSanitizer could not inspect processes in this managed environment, so that run
used `ASAN_OPTIONS=detect_leaks=0`. Address/undefined-behavior checks passed on all
20 cases; leak checking remains unverified. Do not use sanitizer builds to benchmark.

### Decision

Keep Python as the reference specification. The compiled interpreter passes the
current conformance checks and is worth measuring on the Mac. Mac results should
precede decisions about a production core or extending instructions and memory.
The full roadmap still requires translation experiments, richer workloads and a
same-Mac VM baseline before claiming a performance improvement over Docker.

Measurement references:
- [Python resource accounting](https://docs.python.org/3/library/resource.html)
- [Apple getrusage manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html)

<a id="m3"></a>

## Milestone 3: freestanding C program and Linux startup stack

The C backend now runs a small independently compiled ARM64 Linux C program
that reads arguments, uses global writable memory and local stack memory,
computes a digit sum, prints the result, reads its environment, and returns an
input-dependent exit status. Guest behavior is executed from its ELF instructions.

### Mac acceptance

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
./build/dockiva-runtime run examples/m3/demo.elf --env MODE=demo -- 12345
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

### New execution support

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

### Initial stack contract

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

### Validation and independent reference

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

### Rebuild guest fixtures

```sh
python3 scripts/build_m3.py --zig /absolute/path/to/zig
```

Tested with Zig 0.13.0/LLVM. The script records source hashes, ELF hashes and flags.
The C demo is compiled freestanding at O0, without libc, compiler runtime checks,
vectorization or stack-protector helpers. An assembly entry stub reads the initial
stack and calls the ordinary C function. This deliberately constrained build does
not establish compatibility with arbitrary C compiler output or Linux applications.
The supplied ELF files were independently built by LLVM, not our Python ELF builder.

### Reference scope and next work

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

<a id="m4"></a>

## Milestone 4: bounded guest allocation

The compiled C backend adds raw Linux ARM64 syscalls brk (214), munmap (215),
and mmap (222). All guest memory is interpreter-owned data. These calls do not
invoke a Linux kernel or create host executable mappings.

### Mac check

Extract into a fresh directory. Requires Python 3.10+ and a local C compiler.
The independently compiled Linux fixtures are included.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m4.py --report mac-m4-validation.json
python3 scripts/verify_m3.py
python3 scripts/verify_m2.py
```

Expect 17 M4 PASS lines, 22 M3 PASS lines and 20 M2 PASS lines.
`Linux reference: not-run` is expected for Mac verification.

```sh
./build/dockiva-runtime run examples/m4/demo.elf
```

Expected stdout: `heap allocation OK` followed by a newline. Exit status: 0.
The guest grows and writes to its brk heap, shrinks it, maps two pages, copies
text across a page boundary, writes that text through the syscall adapter, and
unmaps the memory. It is freestanding C using raw syscalls, not libc malloc/free.

### Supported contract

| Operation | Implemented behavior |
|---|---|
| brk(0) | Return the current break |
| brk(address) | Grow/shrink within an 8 MiB range following the page-aligned ELF end |
| brk failure | Preserve and return the previous break, following the raw Linux syscall convention |
| mmap | Private anonymous mappings only, flags exactly 0x22; choose an address in a fixed 16 MiB arena |
| mmap protections | Explicit PROT_NONE, READ, WRITE, or READ|WRITE; no executable mappings |
| mmap length | Nonzero, rounded up to 4 KiB; bounded by available arena pages |
| munmap | Page-aligned start inside the arena, nonzero length rounded to pages; partial ranges and holes allowed |
| Reallocation | Freed mapping pages can be reused; new mappings are zero-filled |
| Access checking | Deny accesses to unmapped pages or pages without the requested permission |

The mmap arena begins at guest address 0x1000000000. ELF segments overlapping it
are rejected. Non-fixed address hints are currently ignored. Anonymous fd is
ignored; offsets must be page-aligned, and offset zero is the intended use.
Shared, file-backed, fixed-address and other flags return -EOPNOTSUPP (95).
Executable or unknown protection bits return -EINVAL (22). No mprotect exists.
These unsupported-feature error choices are prototype policy, not full Linux
behavior. PROT_WRITE does not implicitly grant read access in this interpreter.

munmap outside the arena returns -EOPNOTSUPP, including ELF, stack and brk ranges;
it does not implement Linux's ability to unmap arbitrary address-space regions.
Invalid alignment/zero length returns -EINVAL. Oversized or exhausted mmap requests
return -ENOMEM (12) without altering existing mappings. The allocator uses first
fit and needs a contiguous free run; total free pages alone do not ensure success.

The logical break is tracked separately from page-rounded accessibility. A shrink
within a page preserves that page, while regrowing released whole pages produces
zero-filled memory. There is no ASLR. The fixed brk range must fit below the stack
and avoid the mmap arena.

### Host memory and limits

Backing arrays are allocated lazily: up to 8 MiB for brk and 16 MiB for mmap.
Guest release changes accessibility and permits reuse; backing arrays are retained
until runtime exit rather than immediately returned to the host allocator.
These caps are separate from the earlier 16 MiB ELF segment cap and 1 MiB stack.
No reduction in Mac RSS or Docker memory use is claimed.

Accesses crossing pages within the arena are checked against every touched page.
The existing limitation on accesses spanning separate ELF/stack/heap regions
remains. The write adapter prevalidates the full buffer; Linux partial writes
across a faulting range are not emulated.

### Verification

17 independent C fixtures cover:

- Ten Linux-comparable cases: allocation demo, brk shrink/regrow, partial unmapping,
  mapping reuse/zero fill, zero-length mmap, invalid munmap, read-only zero pages,
  PROT_NONE allocation, unmapping holes, and rejecting a break below its base.
- Three runtime-policy cases: 16 MiB mapping exhaustion/recovery, 8 MiB break
  limit, and unsupported mapping/protection flags.
- Four interpreter faults: writing read-only memory, reading after unmapping,
  reading PROT_NONE, and reading a whole page released by brk shrink.

The ten comparable cases match QEMU user-mode on Linux on exact output and exit
status; mapping addresses themselves need not match. Fault and cap policies are
not presented as Linux conformance. M4 Mac acceptance and native ARM64 Linux
hardware comparison remain pending.

```sh
# Optional reference on Linux with QEMU user-mode:
python3 scripts/verify_m4.py --qemu /path/to/qemu-aarch64 --report linux-m4.json

# On native ARM64 Linux:
chmod +x examples/m4/*.elf
python3 scripts/verify_m4.py --native-linux --report arm64-m4.json

# Rebuild fixtures with Zig 0.13.0/LLVM:
python3 scripts/build_m4.py --zig /absolute/path/to/zig
```

The fixtures use a constrained freestanding O0 build with integer registers only,
without libc, compiler runtime checks or stack-protector helpers. Source hashes,
ELF hashes and exact flags are recorded. The runtime build metadata now hashes
both C source and the new heap header.

Python remains the earlier-instruction reference; new allocation behavior is
implemented in the C backend and validated using independent Linux execution.
This milestone does not establish general malloc/libc, dynamic linking, threads,
networking, Docker image compatibility or security isolation.

Implementation references:
- [Linux mmap/munmap manual](https://man7.org/linux/man-pages/man2/mmap.2.html)
- [Linux brk manual and raw syscall return behavior](https://man7.org/linux/man-pages/man2/brk.2.html)

<a id="m5"></a>

## Milestone 5: a Linux HTTP server over real TCP

The compiled C interpreter now adapts a small set of Linux ARM64 networking
syscalls to host sockets. An independently compiled Linux ELF server handles a
real HTTP request from a host client. The HTTP parsing and response are guest C
code; the runtime only interprets instructions and adapts system calls.

### Verify on your Mac

Extract into a fresh directory. Requires Python 3.10+ and Xcode Command Line Tools
(or another local C compiler). The Linux ELF fixtures are included.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m5.py --report mac-m5-validation.json
python3 scripts/verify_m4.py
python3 scripts/verify_m3.py
python3 scripts/verify_m2.py
```

Expect 14 M5 checks, 17 M4 checks, 22 M3 checks and 20 M2 checks to pass. The M5
verifier starts and cleans up its own one-request servers on ephemeral ports.
`Linux reference: not-run` is expected on the Mac command above.

### Try with curl

Prepare two terminals in the extracted project. In terminal 1:

```sh
./build/dockiva-runtime run examples/m5/server.elf
```

It prints `PORT 18080`. Within 30 seconds, run in terminal 2:

```sh
curl --noproxy '*' --max-time 10 http://127.0.0.1:18080/
```

Expected body:

```text
VM-free HTTP server OK
```

The guest server exits with status 0 after one completed response. Start it again
for another request. The guest requests SO_REUSEADDR so the port can be reused.
If 18080 is already in use, start with `-- auto` to request an ephemeral port and
use the printed port in curl instead:

```sh
./build/dockiva-runtime run examples/m5/server.elf -- auto
```

Any extra guest argument currently selects an ephemeral port; this is not a port
number parser. A browser can make extra requests, so curl is the simpler client
for this deliberately one-connection demo. Ctrl+C stops a waiting manual run.

### Protocol scope

The demo responds to GET / with 200, other GET paths with 404, and non-GET methods
with 405. It reads fragmented headers until CRLF CRLF, up to 2048 bytes, and sends
431 when that limit is exhausted. Responses contain Content-Length and close the
connection. A disconnect before complete headers returns guest exit code 4.
It is not a general HTTP parser: no request-body handling, keep-alive, pipelining,
TLS, HTTP/2, concurrency or production robustness is claimed.

### Syscall scope

| Linux ARM64 call | Number | Supported subset |
|---|---:|---|
| socket | 198 | AF_INET, SOCK_STREAM, protocol 0 or TCP; no socket flags |
| bind | 200 | Linux 16-byte sockaddr_in, exactly 127.0.0.1 |
| listen | 201 | Explicitly loopback-bound sockets, backlog 0..128 |
| accept | 202 | NULL peer address and length outputs |
| getsockname | 204 | Marshal Linux sockaddr_in and 32-bit socklen, including truncated buffers |
| setsockopt | 208 | Linux SOL_SOCKET/SO_REUSEADDR, 32-bit integer only |
| read | 63 | Managed sockets into writable guest memory |
| write | 64 | Managed sockets plus the existing stdout/stderr path |
| close | 57 | Managed socket descriptors only |

Linux socket numbers and sockaddr bytes are decoded explicitly. macOS's socket
address layout and option numbers are not assumed to match Linux. Common host
errno values are translated to Linux values; unknown errors fall back to EIO.

### Runtime policies and limits

- Separate guest descriptor table, 32 sockets maximum; host descriptor numbers
  are never returned as guest descriptors. Socket cleanup occurs on normal exit
  or runtime errors; OS process cleanup handles termination.
- Localhost binding only; wildcard and other addresses are denied. listen on an
  unbound socket is denied to prevent implicit wildcard binding. These are scope
  limits, not evidence of a complete security sandbox.
- Blocking sockets. Adapter timeouts: 30 seconds waiting for accept and 5 seconds
  for accepted-socket I/O, implemented through host socket timeout options. These
  are prototype policy; they differ from Linux defaults. The verifier separately
  enforces process/client timeouts and cleans up failed runs.
- Per-call socket I/O capped at 1 MiB. Guest buffers are validated in full before
  host I/O; Linux partial-fault behavior is not fully reproduced.
- SIGPIPE is ignored so socket write failures return errors instead of killing
  the runtime; guest Linux signal delivery is not implemented.
- No connect, DNS, UDP, IPv6, nonblocking I/O, poll/epoll, accept4, arbitrary socket
  options, stdin reads, file descriptors for disk files, or shutdown syscall.
- The instruction budget does not measure wall time spent in a blocking syscall.

### Validation

Six end-to-end server scenarios: root, missing route, method rejection,
fragmented headers, oversized headers, and early disconnect. Eight additional
probes cover descriptor errors/lifetime, bad pointers, localhost policy,
unsupported UDP, descriptor capacity and implicit-bind rejection.

Nine checks (the six server scenarios plus three unambiguous descriptor/pointer
checks) match QEMU user-mode on Linux using the same ELF programs. Five checks are
explicit runtime-policy checks. In particular, simultaneous invalid descriptor
and invalid pointer error ordering is not asserted to match every Linux reference.
M5 Mac execution and native ARM64 Linux hardware comparison remain pending.

```sh
# Optional reference on Linux:
python3 scripts/verify_m5.py --qemu /path/to/qemu-aarch64 --report linux-m5.json

# Reference on ARM64 Linux hardware:
chmod +x examples/m5/*.elf
python3 scripts/verify_m5.py --native-linux --report arm64-m5.json

# Rebuild fixtures with Zig 0.13.0/LLVM:
python3 scripts/build_m5.py --zig /absolute/path/to/zig
```

The fixture build is freestanding O0 C with integer registers only, no libc,
compiler runtime checks or stack-protector helpers. Source and ELF hashes are
recorded. The active networking adapter is native/net.h. This is still a small
compatibility experiment, not arbitrary Linux or Docker image support.

References:
- [Linux ARM64 generic syscall definitions](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)
- [Apple socket documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/socket.2.html)

<a id="m6"></a>

## Milestone 6: local OCI image execution

This release adds `oci.py`, a Python standard-library adapter that validates a
local OCI image layout, merges supported layers in memory, extracts a private
temporary tree, and passes the selected ELF to the existing C interpreter.
The HTTP implementation is still the Linux guest program from M5.

### Mac acceptance

Extract the complete release into a fresh directory, then:

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m6.py --report mac-m6-validation.json
python3 scripts/verify_m5.py
python3 scripts/verify_m4.py
python3 scripts/verify_m3.py
python3 scripts/verify_m2.py
```

M6 expects 24 PASS lines: 18 loader unit tests and six integration checks. These
cover digest/DiffID integrity, extraction, invalid paths and links, whiteouts,
opaque directories, replacement, case collisions, size/member limits, config and
platform errors, inspect, two HTTP responses, Cmd/Env, Cmd override, and stopping
with cleanup. Existing regression counts are 14, 17, 22 and 20 respectively.
The M6 suite is adapter validation, not comparison with another OCI runtime.

```sh
python3 oci.py inspect examples/m6/http-image --ref demo
python3 oci.py run examples/m6/http-image
```

The server prints `PORT 18080`; within 30 seconds, in another terminal:

```sh
curl --noproxy '*' --max-time 10 http://127.0.0.1:18080/
```

Expected body: `VM-free HTTP server OK` plus a newline. It exits after one request.
Append `-- auto` to the run command to choose an ephemeral port instead.

```sh
python3 oci.py run examples/m6/args-image
python3 oci.py run examples/m6/args-image -- 999
```

The first prints `sum=15` and `MODE=oci`, exiting 15. The second prints `sum=27`
and `MODE=oci`, exiting 27. Nonzero statuses here are intentional guest results.
Arguments after `--` replace image Cmd and retain Entrypoint. No shell is invoked.
The native CLI's new `--argv0 NAME` preserves the image executable's guest name.

### Accepted subset

- Local layout version 1.0.0, index schema 2, exactly one selected OCI image
  manifest. Use `--ref` to select the `org.opencontainers.image.ref.name` annotation.
- Linux/ARM64 config and, when supplied, matching descriptor platform.
- SHA-256 blob digest and size verification, plus uncompressed layer DiffIDs.
  Digests detect inconsistent content; they do not authenticate the publisher.
- Uncompressed tar and gzip tar layers; regular files and directories. Empty
  regular-file whiteouts and opaque directory markers remove lower-layer content
  before entries from their own layer are applied.
- Printable ASCII paths, no traversal, absolute paths, backslashes, colons,
  duplicate paths within a layer, or case collisions. Links, sparse files,
  devices and FIFOs are rejected. Ownership, timestamps and extended attributes
  are not reproduced. Files become 0600 or 0700 according to executable bits;
  directories become 0700.
- Absolute ELF entrypoint, Entrypoint/Cmd arrays and explicit Env. No guest PATH
  lookup or host environment import. Default command must itself be valid even
  when Cmd will be overridden. Full ELF validation remains in the C loader.
- Empty or `/` WorkingDir only; no User or Volumes settings; default/SIGTERM
  StopSignal only. This does not implement guest filesystem or identity semantics.
- SIGTERM/SIGINT forwarded to the host interpreter process, with temporary-tree
  cleanup after exit. This does not implement Linux guest signal handlers.

Limits: 1 MiB JSON, 32 MiB per stored blob, 32 layers, 64 MiB total expanded tar
bytes, 16 MiB per file, 4096 tar members and merged paths, 1024-character paths,
32 path components, 64 argv entries including argv0, 64 environment entries,
and 64 KiB combined argument/environment strings. Existing native limits apply.
Errors detected by the adapter exit 125; interpreter errors retain its status.
`inspect` verifies the image and reports its command; it does not execute or fully
validate the ELF instruction stream.

### Boundaries

Use trusted bundled images. This is image packaging support, not a secure
container boundary or a conforming OCI runtime. The temporary rootfs is a source
of the executable; guest filesystem syscalls and rootfs mediation remain future
work. No chroot, Linux namespaces, resource isolation or identity changes occur.
Local layout files must not be modified concurrently during loading; symlink
checks are not a race-resistant filesystem capability system. Abrupt SIGKILL or
host failure can leave temporary files.

No registries, image pulls, Docker save archives, nested indexes, zstd layers,
dynamic linker, general libc, Docker API or arbitrary Linux-image compatibility.
The Python adapter adds startup overhead; no Docker/VM performance comparison
has been performed. M6 passed on Linux x86-64 here; Mac acceptance remains pending.
M5's 14 checks and HTTP response passed on the user's Mac per their report.

### Reproduction and references

`python3 scripts/build_m6.py` regenerates both deterministic OCI layouts from the
bundled M3/M5 ELF fixtures. Their original sources, compiler flags and hashes are
included under those milestones. No Linux toolchain is needed to rebuild the
packaging or run the Mac checks.

OCI specifications consulted for this implementation:

- [Image layout](https://github.com/opencontainers/image-spec/blob/main/image-layout.md)
- [Image configuration](https://github.com/opencontainers/image-spec/blob/main/config.md)
- [Filesystem layers](https://github.com/opencontainers/image-spec/blob/main/layer.md)

<a id="m7-10"></a>

## Combined Milestones 7–10

M7–M10 build on the Mac-validated M6 image loader. The release includes a
root-relative file adapter, private writable container trees, a conventional
static musl-linked program, and a local lifecycle CLI. There is no Linux VM.

### Acceptance on your Mac

Extract the complete ZIP into a fresh directory. Python 3.10+ and the local C
compiler are required. Linux guest binaries and OCI layouts are already bundled.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m7_10.py --report mac-m7-10-validation.json
python3 scripts/verify_m6.py
python3 scripts/verify_m5.py
python3 scripts/verify_m4.py
python3 scripts/verify_m3.py
python3 scripts/verify_m2.py
```

Expect 40 combined PASS lines, followed by 24, 14, 17, 22 and 20 from the previous
milestones. The combined test uses temporary state and cleans up its instances.
Mac execution has not been run here; the user supplies that acceptance result.

### M7: read files from an image

```sh
python3 oci.py run examples/m7_10/http-image
```

In another terminal, within 30 seconds:

```sh
curl --noproxy '*' --max-time 10 http://127.0.0.1:18080/
```

The response contains `Hello from the image filesystem!`. The guest reads
`/www/index.html` through Linux openat/read/close and constructs the HTTP header.
The server handles one request and exits. Append `-- auto` to use an ephemeral
port, then use the printed port for curl. Foreground `oci.py run` uses a temporary,
read-only guest filesystem; its extraction is removed on exit.

The C CLI now accepts `--root DIRECTORY` and optional `--writable`. Image commands
supply roots automatically. Avoid pointing writable execution at important host
directories: use the lifecycle tool's private image copies.

### M8: independent writable state

```sh
python3 containers.py create counter-a examples/m7_10/counter-image
python3 containers.py create counter-b examples/m7_10/counter-image
python3 containers.py start counter-a
python3 containers.py inspect counter-a
python3 containers.py logs counter-a
```

After `inspect` shows `exited`, start counter-a again. Its log now contains `1`
then `2` on separate lines. Starting counter-b produces `1`; its filesystem is
independent. Logs append across restarts. The original OCI blobs are unchanged.

Each container gets a full private copy of the merged image, not a copy-on-write
or overlay mount. State persists after exit/stop and is removed by `rm`.
`create NAME IMAGE --read-only` disables guest writes. `--arg VALUE` can be repeated
to replace image Cmd. Host state defaults to `~/.vmfree/containers`, or use
`--state-dir DIRECTORY` before the action. The state directory must be owned by
the current user and have permissions 0700.

### M9: an ordinary static musl program

```sh
./build/dockiva-runtime run examples/m9/libc-demo.elf
python3 oci.py run examples/m7_10/libc-image
```

The first prints `static musl OK`. The second prints `hello`, read from its image.
`examples/m9/libc-demo.c` uses standard headers, main, strcmp, open, read, write,
close and errno. It is linked normally against Zig's musl with its real startup
code; there is no custom entrypoint or vmfree syscall wrapper in this program.

Four application cases (startup, file reading, ENOENT and invalid arguments),
plus an independent assembly instruction probe, match QEMU user-mode on Linux.
The reference file-read invocation uses an equivalent host file path because the
interpreter maps guest `/` to its root; QEMU itself is not a rootfs sandbox.

New execution support includes logical operations/masks, bitfields, extended
addition, multiply/high multiply, conditional compare, TPIDR_EL0 storage, and a
small set of vector byte duplication/register-transfer/load/store operations
used by musl. DCZID reports that DC ZVA is prohibited. This is not general SIMD,
floating-point, all-musl, malloc, stdio, dynamic-linker or thread compatibility.
Linux set_tid_address returns a single guest thread ID; no thread creation,
futex waiters, clear-on-exit notification or guest signal delivery is implemented.

### M10: lifecycle

```sh
python3 containers.py create web examples/m7_10/http-image --arg auto
python3 containers.py start web
python3 containers.py list
python3 containers.py inspect web
python3 containers.py logs web
python3 containers.py logs web --stderr
python3 containers.py stop web
python3 containers.py rm web
```

`start` is detached and confirms process launch, not application readiness.
Use the server's `PORT` log line as readiness. A supervisor owns each child,
records its exit status, and holds an exclusive lifecycle lock. Starting twice
or removing a running instance fails. `stop` is idempotent after normal exit.
Stopping a running guest terminates its interpreter; the recorded status is 143
for SIGTERM, or 137 if a forced kill is needed after three seconds.

The supervisor listens on an ephemeral IPv4 loopback control port. Stop requests
require a random 256-bit token stored in the private state directory; it is not
printed by inspect or logs. This avoids using a stored PID as authority to kill
a process. Unauthenticated stop requests are tested and ignored. Guest TCP
listeners and supervisor control endpoints have separate purposes.

No automatic reboot recovery or application restart policy is provided. If a
supervisor is killed abruptly, stale running state blocks start/removal and needs
manual inspection; do not assume the guest has exited. Do not manually edit state
while an instance is running. `rm` intentionally deletes that instance's files
and logs. This is a CLI with per-instance supervisors, not a Docker API daemon.

### Filesystem contract and limits

Supported Linux ARM64 calls:

| Call | Scope |
|---|---|
| openat (56) | Root-relative or an opened guest directory; regular files/directories |
| read/write (63/64), close (57) | Separate file descriptor table; sockets retain M5 behavior |
| fstat (80) | Explicit 128-byte ARM64 layout; synthesized ownership/mode/time fields |
| lseek (62) | SET, CUR, END for regular file descriptors |
| ftruncate (46) | Bounded length changes in writable mode |
| mkdirat (34), unlinkat (35) | Directory creation, file deletion, empty directory deletion |
| renameat (38) | Rename within one private root |

Open flags supported: access mode, CREAT, EXCL, TRUNC, APPEND, DIRECTORY,
NOFOLLOW, CLOEXEC and LARGEFILE with explicit ARM64-to-host translation. Other
flags fail. Absolute paths refer to the guest root; relative AT_FDCWD paths use
that root too. Guest chdir/getcwd, directory enumeration, links, chmod/chown,
mounts, devices, file-backed mmap and filesystem permissions/metadata fidelity
are not implemented. fstat uses virtual mode/ownership and zero timestamps;
it must not be treated as a host stat structure.

Every ancestor is opened relative to a directory descriptor with O_NOFOLLOW;
`..`, links, non-ASCII names, backslashes and colons are rejected. Path handling
is a restricted normalized subset rather than complete Linux resolution.
Only trusted images and private trees produced by this loader are in scope.
Concurrent host mutation, host-created hard links, local state tampering and
hostile same-user processes are outside this prototype's boundary.

File descriptors: 32 slots (guest IDs 64–95), separate from 32 socket slots.
Per file: 16 MiB. Per read/write: 1 MiB. Per execution: cumulative writes and
requested truncate sizes total at most 64 MiB; at most 4096 create-open/mkdir
operations. These are conservative operation budgets, not aggregate persistent
disk quotas. Budgets reset on restart. Managed execution uses 10 million
instruction steps. M4 memory and M5 socket/time limits remain unchanged.
Logs are not rotated or assigned disk quotas. No production isolation claim.

### Evidence and reproduction

The 40 combined checks passed on Linux x86-64. The first 39 also passed with
AddressSanitizer and UndefinedBehaviorSanitizer; the final control-authentication
check is Python supervisor behavior. Leak detection was disabled because this
environment cannot inspect process tasks. Previous M2–M6 suites passed.
Reports are under `evidence/m7-10-*`. Native ARM64 Linux hardware remains untested.

```sh
python3 scripts/build_m7_10.py --zig /path/to/zig
python3 scripts/verify_m7_10.py --qemu /path/to/qemu-aarch64-static
```

The reference command is Linux-only. Zig 0.13.0 builds the bundled fixtures;
source/binary hashes and exact build commands are recorded. The normal Mac
workflow needs neither Zig nor QEMU. See `third_party/NOTICE.md` for musl licensing.

References:

- [Linux generic stat ABI](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/stat.h)
- [Zig 0.13.0 bundled musl](https://github.com/ziglang/zig/tree/0.13.0/lib/libc/musl)

<a id="m11-17"></a>

## Combined M11–M17 release candidate

This release implements the networking, image-distribution, Docker API and
Compose subsets below, plus hardening, benchmarking and Mac packaging tooling.
It does NOT close the independent security review, same-Mac VM/resource comparison,
or signed clean-Mac distribution gates.

| Milestone | Delivered | Acceptance status |
|---|---|---|
| M11 | Nonblocking sockets, ppoll, accept4, loopback connect/shutdown, persistent HTTP demo | Linux checks pass; Mac pending |
| M12 | Verified OCI pull/cache, ARM64 index selection, import/inspect/list | Local Distribution API checks pass; public HTTPS/auth/CDN acceptance pending |
| M13 | Resource limits, authenticated control, rejection tests, implementation review | Production isolation review OPEN |
| M14 | Comparable HTTP workload and timing harness; Linux-only measurements | Same-Mac VM comparison and CPU/RSS accounting OPEN |
| M15 | Authenticated Engine API subset | Docker CLI 27.5.1 workflows pass on Linux; Mac pending |
| M16 | JSON Compose subset runner plus actual Docker Compose host-network example | Docker Compose 2.32.4 workflows pass on Linux; Mac pending |
| M17 | Mac packaging/signing/notary scripts, installer and diagnostics | Signed installation, zero-dependency packaging, updates/rollback acceptance OPEN |

### Mac acceptance

Extract the full source package into a fresh directory. Keep the Mac feature-macro
fix from M7–M10; it is already included.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m11_17.py --report mac-m11-17-validation.json
python3 scripts/verify_m7_10.py
```

Expect 30 new PASS lines and 40 from M7–M10. The new suite uses temporary state,
local HTTP registry fixtures and a token-authenticated loopback API. It needs no
Docker Desktop, registry account or Linux toolchain. It removes its own instances.

Optional checks against locally installed client executables:

```sh
python3 scripts/verify_m11_17.py \
  --docker /path/to/docker \
  --compose /path/to/docker-compose \
  --report mac-m11-17-clients.json
```

Docker CLI adds nine checks and Docker Compose adds six. The Compose executable
must be the standalone v2 binary (the same binary can serve as a CLI plugin).
The Linux-only `--qemu /path/to/qemu-aarch64-static` adds one network ABI check.
All options together yielded 46 PASS lines here. Linux sanitizer validation covers
the 30 default checks; leak detection was disabled due to host inspection limits.

### Start the local engine and run two services

First import the bundled supported image:

```sh
python3 vmfree.py image import vmfree/server:demo examples/m11_17/http-image
python3 vmfree.py engine
```

The Mac engine defaults to a private Unix socket. If Unix sockets are unavailable,
use `python3 vmfree.py engine --tcp 0`; this still binds only to 127.0.0.1 and
requires the same token. Run it in its own terminal. Ctrl-C stops the API server;
running containers retain their supervisors. Stop workloads before shutting down
if you want all guest processes to exit.

In a second terminal:

```sh
python3 vmfree.py compose -f examples/m11_17/compose.json up
curl --noproxy '*' --max-time 10 http://127.0.0.1:18081/
python3 vmfree.py compose -f examples/m11_17/compose.json ps
python3 vmfree.py compose -f examples/m11_17/compose.json stop
python3 vmfree.py compose -f examples/m11_17/compose.json up
python3 vmfree.py compose -f examples/m11_17/compose.json down
```

The frontend on 18081 connects to the backend on 18082 and relays its HTML.
The response includes `Persistent VM-free HTTP server OK`. The backend increments
its private `/counter` on startup. Stop/up retains that file; down deletes the
containers and their private data. Named volumes are NOT implemented. Choose other
explicit ports in the example if these are occupied; ports are shared with the host.

The bundled runner accepts JSON syntax (a valid YAML subset), exec-form command
arrays, string environment maps, read_only, depends_on lists, image names and
explicit network_mode: host. It rejects other Compose features before making
changes. Dependencies control start order, not readiness. `up` is idempotent for
unchanged services; changed configs require explicit down/recreation. Failed starts
trigger best-effort cleanup of newly created instances.

### Use the real Docker CLI

The wrapper targets this engine with its private authentication config and API
version 1.45. It does not change your default Docker context.

```sh
python3 vmfree.py docker version
python3 vmfree.py docker images
python3 vmfree.py docker create --network host --name web vmfree/server:demo 18080
python3 vmfree.py docker start web
python3 vmfree.py docker ps -a
python3 vmfree.py docker inspect web
python3 vmfree.py docker logs web
python3 vmfree.py docker stop web
python3 vmfree.py docker rm web
```

These commands require the Docker CLI, but not Docker Desktop. Tested here with
Docker CLI 27.5.1. Other versions remain acceptance targets, not assumed compatible.
If your CLI discovers its Compose plugin:

```sh
python3 vmfree.py docker compose -f examples/m11_17/compose.json up -d
python3 vmfree.py docker compose -f examples/m11_17/compose.json ps -a
python3 vmfree.py docker compose -f examples/m11_17/compose.json stop
python3 vmfree.py docker compose -f examples/m11_17/compose.json down
```

The real Compose 2.32.4 binary passed up/ps, service traffic, stop, up again and
down against this API on Linux. That does not imply general Compose compatibility.
Custom Docker client config directories can affect plugin discovery; the bundled
`vmfree.py compose` runner is available without installing a plugin.

Supported API families: ping/version/info; cached image list/inspect and scoped
pull; container create/list/inspect/start/stop/restart/remove; non-streaming logs;
not-running wait with a 30-second cap. The advertised API range is 1.24–1.45 for
these endpoints only. Unsupported endpoints/options return errors, including
attach/exec/build, TTY/stdin, streaming log follow, namespaces, volumes/mounts,
bridge/DNS networking, port publishing/remapping, privileged execution, custom
resource settings and arbitrary Linux images. `docker run -d --network host`
may use these basic endpoints; the acceptance suite uses explicit create/start.

### Registry pulls

```sh
python3 vmfree.py image pull registry.example.com/team/supported-image:tag
python3 vmfree.py image inspect registry.example.com/team/supported-image:tag
```

Replace the example with a registry containing a compatible image. This release
has not published your image to an external registry. Use an explicit registry
hostname; Docker Hub shorthand is not expanded. HTTPS is required. For an explicit
local test registry only, `--insecure-local` permits literal 127.0.0.1 HTTP.

Supported: OCI manifests/configs, one-level OCI indexes selecting exactly one
linux/arm64 manifest, tar/gzip layers, SHA-256 descriptor and DiffID verification,
bounded downloads, anonymous Bearer challenges over HTTPS, or an explicit token
read from a named environment variable via `--token-env VARIABLE_NAME`.
No credentials are copied into the cache. Redirects/CDNs, Basic auth, credential
helpers, resumable downloads, Docker schema media types and publisher signatures
are not supported. Normal large distro images often exceed the loader's feature
and size limits. Pull validates compatibility metadata; it does not execute a guest.

Each cache name is immutable: re-pulling the same digest is allowed, but changing
an existing name to another digest fails. Use a new tag/digest reference instead.
There is no general tag-update/eviction/delete CLI yet. Cache entries contain only
verified referenced blobs; unreferenced source files and links are not imported.

### Networking and resource scope

M11 supports IPv4 TCP SOCK_NONBLOCK/CLOEXEC, fcntl GETFL/SETFL for sockets,
accept4 with null peer outputs, ppoll over up to 32 guest socket entries, connect
to 127.0.0.1 and shutdown. ppoll has a maximum 30-second wait and rejects signal
masks. No UDP, external networking, epoll, DNS or guest signal implementation.

The demo multiplexes eight clients, handles fragmented requests and partial normal
response writes, and closes each HTTP connection. It accepts GET / only; unsupported
requests get a minimal 400 response. Error responses are best effort. The upstream
relay uses bounded blocking calls; this is not a production proxy or HTTP stack.
Managed execution retains the 10-million-instruction budget, so persistent here
means repeated requests within that budget, not an unlimited server lifetime.

Host limits added to managed guests: 60 CPU seconds, no core dumps, 128 host file
descriptors and 16 MiB per host file (including each output log). These supplement
existing guest memory/file/write budgets; they do not establish production isolation.

### Remaining gates and reproducibility

- [M13 review and threat model](#security-review-m13)
- [M14 benchmark setup and limitations](#m14-benchmark)
- [M17 Mac packaging and open distribution requirements](#mac-distribution-m17)

`python3 scripts/build_m11_17.py --zig /path/to/zig` rebuilds the guest fixtures
with Zig 0.13.0. Original C/assembly, hashes and command flags are included.
The downloaded Docker and Compose validation binaries are not bundled.

References used for interoperability:

- [Docker Engine API 1.45](https://docs.docker.com/reference/api/engine/version/v1.45/)
- [Docker client headers/config](https://docs.docker.com/reference/cli/docker/#custom-http-headers)
- [Compose specification](https://docs.docker.com/reference/compose-file/)
- [OCI Distribution specification](https://github.com/opencontainers/distribution-spec/blob/main/spec.md)

<a id="security-review-m13"></a>

## M13 security review and open acceptance gate

This is an implementation review of the prototype, not an independent security
audit or approval to run hostile workloads. The production-isolation acceptance
gate remains OPEN.

### Intended boundary

Trusted, supported Linux ARM64 guest ELFs are interpreted as data. Guest syscall
numbers are dispatched explicitly, never passed through to the host kernel.
A container has a private rootfs copy and a bounded guest descriptor table.
The manager and image cache belong to one local OS user. Other processes running
as that user, root, concurrent host mutation and compromised host libraries are
outside the present boundary.

Network policy is host loopback IPv4 TCP only, for both bind and connect. This
allows service communication but also lets a guest contact other local services.
It is NOT per-container network isolation or an allowlist of destination ports.
No registry image is executed merely because it was downloaded.

### Changes and reviewed findings

| Area | Result |
|---|---|
| Registry import | Fixed a potential unreferenced-link copy: import now copies only verified manifest/config/layer blobs. A regression fixture includes a link to an unrelated host file. |
| Registry transport | HTTPS verification uses the platform trust store. Plain HTTP requires explicit opt-in and literal 127.0.0.1. Redirects fail instead of forwarding credentials. |
| Registry authentication | Explicit Bearer token or anonymous HTTPS Bearer challenge. Tokens stay out of cache metadata and diagnostics. Empty, oversized and newline-bearing tokens are rejected. |
| Image integrity | Requested manifest digest, descriptor sizes/hashes and uncompressed DiffIDs are checked before commit. Digest integrity does not authenticate a publisher. |
| Engine control | Random token, private client config, loopback TCP or private Unix socket, no CORS, bounded requests and 16 request workers. Unauthorized requests fail. |
| Filesystem | Root-relative directory descriptors, no-follow traversal, private copies, write budgets and earlier path/link rejection tests. Guest links/devices/mounts are unsupported. |
| Process authority | Supervisor-owned child lifecycle; a PID in metadata is not sufficient authority to stop a process. Running-instance locks and stop tokens remain enforced. |
| Host resource limits | Managed child: no core dumps, 16 MiB maximum file size (also caps each log file), 128 descriptors, 60 CPU seconds. Existing guest instruction/memory limits remain. |
| Unsupported API settings | Privileged mode, host mounts, custom security settings, port remapping and unsupported platforms fail. |

### Remaining risks and work

- The C interpreter and adapters are trusted native code. A memory safety defect
  could expose the host process. Sanitizer success is not proof of isolation.
- No verified macOS Seatbelt/App Sandbox boundary or Linux seccomp policy exists.
  Resource limits are not a filesystem/network sandbox.
- No aggregate persistent-disk quota, container CPU fairness or guest memory
  accounting across all containers exists. Restart resets per-process budgets.
- Host-created hard links, same-user tampering and concurrent host rootfs changes
  are not protected by a complete capability model. Only loader-created private
  trees are supported. Mac case-insensitive filesystem semantics also differ.
- Cached names are immutable until managed manually; update/eviction policy,
  publisher signatures, credential helpers and registry redirect/CDN support
  remain open. Bearer token servers are trusted as directed by the chosen registry.
- The HTTP demo has eight client slots and no per-client idle deadline. Slow
  clients cannot block unrelated reads within available slots, but can occupy
  all slots. Its upstream relay uses bounded blocking I/O, not a fully asynchronous
  proxy. This is not a public-facing production HTTP implementation.
- Abrupt supervisor failure can leave a running process with stale metadata.
  Automatic recovery, stronger process identity and transactional crash repair
  still need design and adversarial tests.
- Registry downloads use HTTPS in production code, but this release's automated
  registry fixture is local HTTP. Public authenticated/CDN registries are not an
  end-to-end acceptance result.

### Evidence and required next review

The combined suite exercises malformed/unsupported control requests, unauthenticated
access, corrupted registry blobs, link imports, networking/error paths and existing
filesystem/OCI rejection cases. Native paths pass AddressSanitizer and
UndefinedBehaviorSanitizer with leak detection disabled by environment limitation.

Before untrusted-image support: commission an independent review, define a tested
OS confinement policy, add mutation/race and parser fuzz campaigns, close aggregate
resource gaps, and test denial-of-service and crash recovery on the target Mac.

<a id="mac-distribution-m17"></a>

## M17 macOS distribution tooling

This source release includes a Mac packaging script, a versioned user-space
installer, a rollback-friendly `current` symlink, and `vmfree.py doctor`.
It does not include a signed/notarized Mac executable built in this Linux session.
The signed-install acceptance gate remains OPEN.

### Build on the Mac

```sh
python3 scripts/package_macos.py --output dist/vmfree-macos.zip
```

This builds and verifies the runtime, then uses `ditto` to create a distribution
with the Mac executable and Python entrypoints. The end user needs Python 3.10+;
a Python runtime is not bundled. This therefore does not yet meet a clean-Mac,
zero-dependency distribution goal. No GUI or automatic updater is included.

To sign and submit, use your own Developer ID Application identity and an
already configured notarytool keychain profile:

```sh
python3 scripts/package_macos.py \
  --identity 'Developer ID Application: YOUR NAME (TEAMID)' \
  --notary-profile YOUR_EXISTING_PROFILE \
  --output dist/vmfree-macos.zip
```

The script signs/verifies the native executable with hardened runtime and submits
the ZIP to Apple's notarization service. It does not create or transmit credentials
in source files. A ZIP cannot be stapled; a future app/DMG/PKG packaging flow is
needed for a stapled offline distribution. Signing the native executable alone
does not authenticate editable Python sources as an application bundle.

After extracting that Mac-built ZIP:

```sh
python3 vmfree/install.py
python3 ~/.local/share/vmfree/current/vmfree.py doctor
python3 ~/.local/share/vmfree/current/vmfree.py engine
```

The installer uses `~/.local/share/vmfree/0.17.0` and selects it through `current`.
It does not overwrite the same version, change your Docker context, install a
login item, or remove runtime state in `~/.vmfree`. Future versions can be installed
in separate version directories. To roll back, stop the engine and select a known
previous installation with a replacement `current` symlink; no automated rollback
command is supplied yet.

Updates are manual versioned installations. There is no update feed, automatic
signature verification of a feed, or automatic crash/reboot recovery. Uninstalling
the CLI directory does not delete your container state. Stop guests before removing
an installation containing their interpreter.

Validate the package on a clean Apple Silicon Mac, check Gatekeeper behavior,
notary status, install/start/stop/update/rollback and removal, and separately decide
how to bundle Python before calling M17 complete.

[Apple notarization documentation](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)

<a id="mac-build-fix-1"></a>

## macOS build fix 1

Enable _DARWIN_C_SOURCE before all headers on Apple platforms, while retaining
_POSIX_C_SOURCE=200809L. The strict POSIX setting added in M7 hid INADDR_LOOPBACK
and O_NOFOLLOW in the user's macOS SDK. No filesystem checks are removed and no
platform flag values are invented.

The Linux build and all 40 combined checks passed after this change. Compilation
and execution on macOS still require user validation. Rebuild using:

    python3 scripts/build_native.py
    python3 scripts/verify_m7_10.py --report mac-m7-10-validation.json

<a id="m14-benchmark"></a>

## M14 performance checkpoint

The harness measures startup-to-first-response, HTTP throughput, median latency
and p95 latency for explicit launcher argument arrays. It validates the response
body on every request. It does not invoke a shell for the configured commands.

Recorded here: three Linux-host interpreter samples, each with 50 requests and
four concurrent clients. See `evidence/m14-linux-runtime-only.json`. This is not a
Mac benchmark and not a comparison with a Linux VM. No performance advantage is
claimed. CPU/RSS and whole-VM resource accounting are not measured by this harness;
the full M14 acceptance gate remains OPEN.

### Same-Mac comparison setup

Use the exact `examples/m11_17/server.elf` and HTML fixture in both backends.
The supplied scratch Dockerfile packages that ELF without rebuilding it. Its CMD
uses `18080 0 --bind-any` to listen on all container interfaces, which Docker
published ports require. The native benchmark omits that option and stays on
loopback. The runtime still rejects wildcard binds. Rebuild the Docker image
after updating the guest; use a fresh private instance for the same guest bytes.

On your Mac, build the comparison image against your existing Linux-VM-backed
Docker engine, explicitly selecting its context (replace the example context):

```sh
docker --context desktop-linux build --platform linux/arm64 \
  -t vmfree-benchmark:local examples/m11_17
```

For the VM-free command, first create a stopped private instance so its rootfs
can be used for benchmarking:

```sh
python3 containers.py create bench-root examples/m11_17/http-image --read-only
```

Create `benchmark-config.json` using your real absolute project/rootfs paths:

```json
[
  {
    "name": "VM-free",
    "kind": "vmfree",
    "command": ["/ABS/PROJECT/build/dockiva-runtime", "run", "/ABS/HOME/.vmfree/containers/bench-root/rootfs/app/server", "--root", "/ABS/HOME/.vmfree/containers/bench-root/rootfs", "--max-steps", "10000000", "--", "0"]
  },
  {
    "name": "Linux VM through Docker",
    "kind": "linux-vm",
    "port": 18090,
    "command": ["docker", "--context", "desktop-linux", "run", "--rm", "--name", "vmfree-bench-vm", "--platform", "linux/arm64", "-p", "127.0.0.1:18090:18080", "vmfree-benchmark:local"],
    "cleanup": ["docker", "--context", "desktop-linux", "rm", "-f", "vmfree-bench-vm"]
  }
]
```

The named Docker container is created by this benchmark and removed afterwards.
Make sure the chosen name and host port are unused. The explicit external port
accounts for Docker's port forwarding; the guest's printed port is internal to
its container. Keep the comparison VM running before the run to measure warm-VM
container startup; measure cold VM startup separately and label it clearly.

```sh
python3 scripts/benchmark_m14.py --config benchmark-config.json \
  --samples 5 --requests 100 --concurrency 4 --report mac-m14-comparison.json
```

Repeat under comparable thermal/load conditions. The launcher overhead is part
of startup. HTTP timings include both execution and the chosen host networking
path. Later collection must separately report guest/runtime CPU, process RSS,
Docker container memory and whole-VM overhead; these counters are not interchangeable.
A backend result without its paired same-Mac baseline cannot establish an advantage.

### Updated Mac evidence and next measurement

See [M14 matched profile](#m14-matched-profile) for the validated sustained
Mac results, matched-rate collection and opt-in interpreter counters. Sustained
Mac performance and process/container resource collection are now evidenced;
whole-VM accounting remains open. Earlier Linux-only notes above are historical.

The next optimization checkpoint is documented in [M14 lookup cache](#m14-lookup-cache), with a same-binary cache on/off comparison.

The instruction-classification cache candidate is documented in [M14 decode cache](#m14-decode-cache). Its Mac performance validation is pending.

<a id="m14-bind-fix"></a>

## M14 published-port fix

The prior Docker benchmark guest listened only on container loopback. Published
ports require a listener reachable through the container network interface.
The updated Docker CMD is `18080 0 --bind-any`. Native execution defaults to
loopback and still rejects wildcard binds. Both backends use the updated ELF.

From the project root after extracting this patch:

```sh
docker --context orbstack build --platform linux/arm64 -t vmfree-benchmark:local examples/m11_17
python3 scripts/setup_m14_bind_fix.py
python3 scripts/benchmark_m14.py --config benchmark-config-bind-fixed.json --samples 5 --requests 100 --concurrency 4 --report mac-m14-comparison-bind-fixed.json
```

The setup script retains Docker command, port, and cleanup from your original
benchmark-config.json. It creates a new stopped, read-only private benchmark
instance and writes a separate config. Keep the published host address at
127.0.0.1. The old benchmark instances remain available.

Local validation: 30 M11–M17 checks; five readiness tests; QEMU Linux HTTP on
127.0.0.2 with wildcard binding; default loopback-only binding; native wildcard
rejection. macOS/OrbStack validation remains pending. No performance comparison
or resource savings are established by these checks.

<a id="m14-decode-cache"></a>

## M14 bounded instruction-classification cache

The interpreter now separates its 31 instruction-class predicates from execution
and dispatches through a standard C switch. A 1,024-entry direct-mapped table,
8 KiB on the tested build, caches the class keyed by the full 32-bit instruction
word. Hits avoid rescanning the classification predicates. Operands and immediates
are still decoded in the execution handlers; this is not a full decoded-block
cache, JIT, or host executable mapping.

Every iteration retains PC alignment, executable permission, full four-byte fetch
bounds and instruction-budget checks. The instruction word is fetched again
before consulting the cache. Modified bytes therefore select their current
classification; collisions compare the full word and replace the slot on a miss.
PC-relative operations continue to use the current PC in their execution handler.
All reserved-encoding and unsupported-instruction checks remain in place.

`--no-decode-cache` selects classification on every instruction in the same
switch-dispatch binary. `--no-lookup-cache` remains an independent memory-lookup
control. The decode benchmark leaves memory lookup caching as configured; the
existing benchmark config enables it in both variants.

### Mac commands

No guest or Docker image rebuild is needed. Rebuild the host runtime:

```sh
python3 scripts/build_native.py
python3 scripts/verify_decode_cache.py
```

Compare CPU at matched load (about 2–3 minutes):

```sh
python3 scripts/benchmark_decode_cache.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 3000 --resources --report mac-m14-decode-cpu.json
```

Compare unpaced capacity separately:

```sh
python3 scripts/benchmark_decode_cache.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 0 --report mac-m14-decode-throughput.json
```

These are native-only, same-binary AB/BA comparisons. Upload both JSON reports.
Do not enable `--profile` for primary performance measurements. Optional separate
profiling reports include `decode_checks` (ordered class predicates evaluated),
`decode_hits` and `decode_misses`. With caching disabled, hits/misses are zero and
all dispatches classify. Counters include startup, warmup and snapshot wait and
measure event frequency, not elapsed decoding CPU time.

Cache-off uses the new classifier and switch, not the pre-change if/else
interpreter. Thus this A/B test isolates caching within the new dispatcher;
comparison against a previous release additionally needs controlled measurements
of that earlier binary. Performance improvements are not established by hit rate
alone. The change remains a candidate until the Mac results are evaluated.

### Validation

- One million deterministic instruction words compared against the frozen original
  predicate ordering, repeated hits, collision churn and fixed special encodings.
- Execution with cache on/off validates modified instructions at the same address,
  PC-relative instructions at different addresses and executable permissions.
- Direct memory-lookup equivalence tests still pass.
- Existing M2, M3, M4, M7–10 and M11–17 checks passed locally after the dispatch
  refactor, including malformed/reserved instructions and memory faults.
- Existing benchmark tests and paired CLI profile smoke completed.
- Local paced HTTP smoke: approximately 98% classification hit rate; approximately
  98% fewer predicate checks. The report is bundled as Linux smoke evidence only.

### Previously validated memory-lookup result

The user's preceding same-binary Mac reports showed 13.7% lower matched-load CPU
(31.67% to 27.33%) and 16.1% higher unpaced throughput (9,848 to 11,436 requests/s)
with lookup caching. That cache stays enabled in this experiment. This does not
predict the additional benefit, if any, from classification caching.

<a id="m14-host-load"></a>

## M14 synchronized host-process measurement

The uploaded 60-second idle measurement succeeded for PID 47448 (OrbStack
Helper), with 117 samples, 0.33384% CPU on a one-core scale, median RSS 512.40625
MiB, and RSS range 512.109375–528.515625 MiB. This is much steadier than the prior
30-second window (507.1–749.8 MiB), although RSS still declined during this run.
It is a selected-process baseline, not proof of all VM memory usage.

This benchmark-only update adds --host-vm-pid. It samples the selected host PID
only during each Docker measured load window, after warmup. Native and container
resource collection are retained. All collectors use the same load end timestamp;
late collector responses are excluded even if stopping another collector takes
time. Identity (start time plus executable name) is checked before and after each
Docker sample to detect restarts/PID reuse. Collection errors are explicit.

After extracting the patch, no native or Docker rebuild is required:

```sh
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 3000 --resources --host-vm-pid 47448 \
  --report mac-m14-host-loaded.json
```

If OrbStack restarted, first run `python3 scripts/measure_vm_process.py --list`
and substitute its current Helper PID. Keep unrelated VM/container workloads
paused. Upload the resulting JSON. The script does not signal, stop or modify
the selected host process; normal benchmark container cleanup remains unchanged.

Each Docker sample includes `host_vm_process` with raw timestamps, RSS, cumulative
CPU, sampled peak RSS and sampled CPU percent. The top-level
`host_process_collection_completed` requires successful collection for all Docker
samples. HTTP completion, achieved target rate and container resource collection
remain separate statuses. Sampling overhead is part of this instrumented run;
use the earlier uninstrumented report for performance comparisons.

Interpret the host-process data alongside the idle baseline, native process and
container data. Do not add container CPU/memory to host helper usage: scopes may
overlap. Host helper RSS may exclude guest-backed memory or other helper processes.
A before/after RSS difference alone is not an allocation measurement. The full
whole-VM accounting gate remains open unless these scopes are independently
established.

Validation: existing benchmark tests plus real local HTTP load with controlled ps
outputs, host PID routing, and rejection of changed process identity before
container launch. Real Mac process metrics remain to be validated by this run.

<a id="m14-lookup-cache"></a>

## M14 immutable-segment lookup cache

The runtime now keeps the most recent immutable ELF/stack segment for each
permission value. A cache hit checks the full access range and permissions
before returning a host pointer. Failed candidates use the original segment
scan and heap lookup. Heap and mmap mappings are never cached, so brk shrink,
unmap, reuse and protection checks always consult current state. Zero-length
accesses bypass the cache to preserve the original segment-boundary ordering.

This is a memory-lookup optimization, not a JIT or an OS isolation change.
`--no-lookup-cache` selects the reference scan path in the same binary. It must
appear before the guest argument separator `--`. Profiling adds
`lookup_cache_hits`; `segment_probes` now includes attempted cached-segment checks
as well as fallback scan checks. Counters count events, not CPU time.

### Mac validation and comparison

Extract into the existing project, then rebuild the native runtime only:

```sh
python3 scripts/build_native.py
python3 scripts/verify_lookup_cache.py
```

Compare process CPU at a matched target rate:

```sh
python3 scripts/benchmark_lookup_cache.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 3000 --resources --report mac-m14-cache-cpu.json
```

Compare capacity without resource polling:

```sh
python3 scripts/benchmark_lookup_cache.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 0 --report mac-m14-cache-throughput.json
```

Both reports compare cache off/on using the same native binary and guest, in
balanced AB/BA order. No Docker backend is launched. Configuration files and
images remain unchanged. The native instruction budget is 10 billion per sample.
Upload both reports. Do not run them simultaneously. The resource comparison is
instrumented; the throughput comparison is uninstrumented. Cache-off has the
reference semantics but shares the new binary layout and a disabled-cache branch;
it is not a byte-identical pre-change binary. `--profile` is available for separate
counter collection and should not be enabled in the primary performance runs.

### Local validation

- Direct cached/reference pointer equivalence across permissions, overflow-sized
  inputs, zero-byte and cross-segment boundaries, holes, brk shrink, mmap unmap
  and PROT_NONE reuse.
- Existing M2, M3, M4, M7–10 and M11–17 validation suites passed locally.
- Paced HTTP smoke with live profile snapshots: cache off/on both completed.
  Approximately 84% of lookups hit; segment probes fell about 39% in that run.
  Slightly different polling/scheduling can change total instruction counts.
- Paired runner smoke verified AB/BA order, both completion statuses and profiles.

Reduced lookup work is established locally. Mac CPU and throughput improvement
remain unproven until the two reports above are collected. No guest binaries,
Docker image, filesystem/network restrictions or process limits were changed.

### Prior Mac host-process evidence

At similar achieved rates (~2,938 native vs ~2,958 Docker requests/s), the previous
instrumented run reported median CPU 31.18% for the native process and 132.21% for
the selected OrbStack Helper. Median-of-run-median RSS was 2.53 MiB vs 706.14 MiB.
The 60-second Helper idle run showed 0.334% CPU and median RSS 512.41 MiB. These
are selected-process measurements and do not establish complete whole-VM resource
accounting. The current optimization should first be judged against its own
cache-disabled native baseline.

<a id="m14-matched-profile"></a>

## M14 matched-rate and counter profiling update

### Validated Mac results

Uploaded sustained report: macOS 26.5.1 ARM64, four AB/BA rounds, 15-second load
windows, 2-second warmup and concurrency 4. All eight samples completed with
1,169,868 validated workload responses. Medians across per-run measurements:

| Metric | VM-free | Docker / OrbStack |
|---|---:|---:|
| Startup to first response | 10.05 ms | 372.42 ms |
| Throughput | 12,443 requests/s | 7,143 requests/s |
| Median latency | 0.307 ms | 0.524 ms |
| Per-run p95 latency | 0.319 ms | 0.773 ms |

The separate instrumented run also completed. Native sampled CPU was a median
94.84% of one core and sampled peak RSS was 2.53125 MiB in every run. Docker's
median-of-run-median CPU reading was 11.28%; reported memory ranged from 936 KiB
to 2.066 MiB. These are different accounting scopes and different achieved loads.
No CPU efficiency, whole-system memory saving, or energy advantage is established.
Startup includes direct native launch versus Docker container creation and
forwarding readiness, with an already-running VM. Supported guest compatibility
and production-security gates remain separate.

### Run the updated checks on Mac

Rebuild only the host runtime; Docker image and private guest roots stay valid:

```sh
python3 scripts/build_native.py
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 3000 --resources --report mac-m14-matched-resources.json
```

The total target is 3,000 requests/s, shared across four workers. The scheduler
spaces requests and drops stale slots instead of issuing catch-up bursts. Each
result includes achieved throughput and target fraction. `matched_rate_achieved`
requires every sample to deliver at least 95% of target; inspect individual
fractions before comparing CPU. This is a paced, bounded-concurrency generator,
not a full open-loop load tool; response latency excludes scheduling delay and
missed slots, and cannot describe overload tails. At very low rates, quantization
of slot counts matters. Resource polling adds overhead.

Run native counter collection separately to avoid adding its instrumentation to
the resource comparison:

```sh
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 2 --duration 10 --warmup 2 --concurrency 4 \
  --rate 3000 --profile --report mac-m14-matched-profile.json
```

`--profile` instruments only the native backend. It counts executed instructions,
permission-checked memory lookups, segment probes and syscall numbers below 512.
Counters are cumulative across startup, warmup, load and snapshot wait, not
load-window-only. A SIGUSR1 request produces a JSON snapshot on stderr. Normal
exit also emits counters. The signal handler only sets a flag; output occurs in
normal execution. Profiling interrupts of the guest network wait are handled as
a host instrumentation event, allowing the guest poll loop to continue. Snapshot
success is separate from HTTP success (`profile_collection_completed`).

These are event counters, not sampled CPU stacks or per-instruction timing. They
can identify frequent operations but cannot prove which operation costs the most
CPU. Instrumentation changes timing. Use the previous uninstrumented report for
the performance claim. Runtime defaults are preserved except for the benchmark's
already-documented explicit 10-billion instruction budget. The new runtime does
add disabled counter checks to its hot path; rerun uninstrumented timings before
claiming unchanged performance after this build.

### Host VM process measurement

List candidates without modifying or stopping any process:

```sh
python3 scripts/measure_vm_process.py --list
```

Send this output to identify the actual VM process. The list is heuristic and may
include helpers. Once the PID is identified, use `--pid` with `--duration` and
`--report` to record an idle baseline, then a separate window overlapping a
Docker-only load interval. Do not label a window containing alternating native
and Docker samples as Docker-only. Keep other VM workloads paused.

The helper records identity, RSS and CPU of the explicitly selected host PID.
It does not automatically claim whole-VM accounting: virtualized guest memory,
shared mappings and helper processes may have different accounting. A selected
process measurement alone may not close the whole-VM gate. No processes are
killed or settings changed by this helper.

### Validation scope

Benchmark unit/integration tests cover pacing and existing resource/error paths.
Native Linux checks validate hello stdout, exact hello syscall counts, paced
HTTP and a live SIGUSR1 snapshot without server exit. The existing M2 and M11–17
regressions were also run. Actual macOS profiling/signal behavior and the new
matched-rate resource run remain to be validated by the commands above.

<a id="m14-sustained"></a>

## M14 sustained comparison

Prerequisite: the M14 bind fix is installed, its Docker image was rebuilt, and
benchmark-config-bind-fixed.json already completed on your Mac. No new image or
native build is required for this benchmark-only patch.

Keep OrbStack running, connect power, and avoid other heavy workloads. Both runs
use the existing configuration and Docker context. Each sample creates a fresh
backend. Four rounds alternate AB, BA, AB, BA, balancing which backend runs first.
Startup includes launcher and host HTTP readiness; this is not cold-VM startup.
The 2-second load warmup is excluded from throughput. Each worker sends sequential
requests until the 15-second deadline; pending requests finish and their drain
time is included. All responses retain the original workload content check.
Requests do not use keep-alive. Each latency percentile is for one sample.

### 1. Performance run (about 2–3 minutes)

```sh
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --report mac-m14-sustained.json
```

### 2. Resource run (about 2–3 minutes)

```sh
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --resources --report mac-m14-resources.json
```

Use the first report for performance comparisons. Resource polling has different
overhead for the two backends and perturbs timing. Upload both JSON reports.
Progress prints before and after every sample. A partial report is saved after
each completed sample and on caught errors. Keyboard interruption also saves
partial results and triggers backend cleanup. Existing cleanup commands are used.

### What resource numbers mean

- Native: `ps` samples direct runtime PID RSS and cumulative CPU time every 0.5s.
  Sampled CPU percent is CPU-time delta / wall-time delta, with 100% representing
  one core. Peak RSS is the largest sampled value, not a true lifetime peak.
  The launch command must directly start the runtime, as the supplied config does.
- Docker: `docker stats --no-stream` targets the configured container name through
  the same context. Raw CPU percent and memory usage/limit are retained. Collection
  is slower than native ps, and sample timestamps reflect command completion.
  The memory value is Docker CLI accounting, not process RSS. Docker documents
  that Linux CLI memory subtracts cache: https://docs.docker.com/reference/cli/docker/container/stats/
- These scopes are different. Do not divide their memory values to claim overall
  memory savings. No whole-VM CPU/RSS, Docker daemon overhead, client CPU, energy,
  or cold VM launch measurement is included. Whole-VM accounting remains open.
- `completed` means the HTTP runs succeeded. For instrumented reports,
  `resource_collection_completed` separately requires at least two samples per
  backend run and no collector errors. Missing access/tools are explicit errors,
  not zero usage. A failed collector does not discard valid HTTP measurements.
- The Python load generator and local networking can limit throughput. This is
  evidence for this HTTP fixture at this concurrency, not arbitrary containers.

The script records the effective command and raises only its native benchmark
instruction budget to 10 billion. Runtime defaults, managed-container limits,
existing config files, and guest binaries are unchanged. Duration accepts 1–60s;
sample count must be even, 2–20; concurrency 1–8. Very long workloads may still
exhaust the finite instruction budget; failures remain explicit.

### Validation included

11 benchmark tests pass, including real duration-based HTTP traffic, invalid
response failure, CPU clock parsing, guest-argument preservation, resource error
reporting, and Docker context retention. Resource parsing/collection tests use
controlled ps/Docker outputs. A three-second Linux native smoke run completed
12,128 validated requests. Local ps access was denied and correctly recorded as
incomplete. Real macOS ps and OrbStack stats collection require your Mac run;
no new macOS performance result is claimed in this patch.

<a id="m18"></a>

## M18: static BusyBox command compatibility

This candidate runs a minimal five-applet BusyBox 1.37.0 build from unmodified
upstream sources. It is statically linked for ARM64 Linux against musl using Zig
0.13.0. The applet code uses -O0 and general registers; selected musl routines
still use SIMD and exclusive instructions. This is a configured upstream build,
not a stock distribution's full BusyBox executable or proof of general image
compatibility. Shells, process creation, pipes and threads remain unsupported.

### Validate on Mac

The archive includes the guest binaries and OCI layout. Rebuild only the host C
runtime; no Linux VM, BusyBox compiler or Docker rebuild is needed to use them.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m18_native.py
python3 scripts/verify_m18.py --report mac-m18-validation.json
```

Try the OCI fixture directly:

```sh
python3 oci.py run examples/m18/busybox-image -- echo 'Hello from BusyBox'
python3 oci.py run examples/m18/busybox-image -- cat /hello.txt
python3 oci.py run examples/m18/busybox-image -- pwd
python3 oci.py run examples/m18/busybox-image -- ls /bin
```

The verification suite also creates a temporary managed container, runs BusyBox
mkdir in its private writable root, restarts it to confirm persistence/error
status, reads stderr logs, and removes the instance. Existing user containers and
images are not used by the tests. `cat` file arguments are supported; stdin/TTY
and arbitrary BusyBox flags/applets are not part of this milestone.

### Runtime changes

- Signed byte/halfword/word loads, integer division, widening multiply-add/subtract,
  variable shifts/rotate, bit/byte reversal and leading-zero/sign counts.
- The exact SIMD zero-immediate form used by musl.
- Single-thread 32/64-bit exclusive load/store and CLREX. This is an interpreter
  reservation model; it does not add threads, futexes or interprocess atomics.
  Stores/syscalls invalidate reservations. Instruction fetch still checks current
  bytes and executable permissions through the existing caches.
- getcwd reports `/` for a configured guest root. No chdir or host cwd exposure.
- Root-relative newfstatat for regular files/directories, with explicit guest ABI
  packing. Symlinks and special files are rejected even with AT_SYMLINK_NOFOLLOW;
  this intentionally differs from Linux's full metadata semantics.
- getdents64 translates host directory entries into aligned Linux records. It
  preserves continuation across buffers, validates guest pointers, limits each
  buffer to 1 MiB and each enumeration to 4,096 entries, and rejects non-ASCII or
  overlong names. Directory cookies are synthetic; only rewind-to-zero is supported.
- Limited file-descriptor fcntl operations needed by musl, non-TTY ioctl responses,
  and writev with at most 16 vectors / 1 MiB total. Regular-file vectors use the
  existing bounded write path and do not promise atomic multi-vector writes.

Existing root-relative no-follow traversal and read-only policy remain active.
There is still no production OS sandbox, and concurrent host tampering remains
outside the current threat model. More syscalls mean more review surface; this
release does not close the security or clean-Mac distribution gates.

### Evidence and scope

The bundled Linux report includes native execution and independent QEMU/Linux
checks for the new instruction probe and all five selected commands. QEMU pwd
expects its real test-directory path while the runtime expects `/`; this deliberate
namespace difference is recorded in the verifier. No Linux VM is used by the
runtime itself. QEMU is optional validation tooling and is not bundled.

Additional direct tests cover directory ABI records, small-buffer continuation,
rewind/close, invalid pointers/flags, symlink/traversal rejection and writev bounds.
Tests also cover both existing caches disabled individually. Prior M2, M3, M4,
M7–10 and M11–17 suites and both cache-conformance tests passed locally. The M18
suite passed with ASan/UBSan, with leak detection disabled in this environment.
Actual Mac compilation and M18 execution are pending the commands above.

The preceding Mac memory-lookup and decode-cache benchmarks are preserved under
evidence. They establish improvements for the earlier HTTP fixture, not BusyBox
performance or performance of every later runtime build.

### Source and rebuilding

Complete upstream source is bundled at
`third_party/busybox/busybox-1.37.0.tar.bz2`, alongside its GPLv2 license. Its SHA256
is pinned in scripts/build_m18.py. The build configuration, compiler adapter,
conformance assembly and fixture hashes are under examples/m18. Existing musl and
Zig license notices are under third_party.

Rebuild guest fixtures on Linux with Python 3.12+, GNU make, a host C compiler and
Zig 0.13.0:

```sh
python3 scripts/build_m18.py --zig /path/to/zig
```

The compiler adapter translates BusyBox dependency-file flags for Zig and omits
unsupported linker diagnostics/map/sort options; it does not edit BusyBox sources.
Build timestamps/toolchain paths can affect byte identity; the manifest records
the resulting artifact hashes. `--package-only` regenerates the OCI layout and
manifest from existing guest binaries. A rebuild is not required for Mac users.

Upstream source: https://busybox.net/downloads/busybox-1.37.0.tar.bz2

<a id="m19"></a>

## M19: bounded guest processes and a basic noninteractive shell

M19 adds interpreted child processes, static ELF replacement, Linux wait-status
packing, and a small freestanding ARM64 shell. The shell is new fixture code;
this does **not** establish BusyBox ash or general POSIX shell compatibility.
The existing unmodified-source M18 BusyBox executable supplies external commands.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m19_native.py
python3 scripts/verify_m19.py --lifecycle --report mac-m19-validation.json
python3 oci.py run examples/m19/shell-image -- -c 'busybox echo hello; probe exit 7; status'
python3 oci.py run examples/m19/shell-image -- -c 'false && busybox echo skipped || busybox cat /hello.txt'
```

### Process contract

- ARM64 `clone(SIGCHLD, 0, 0, 0, 0)` copies registers, stack, ELF memory, brk and
  anonymous mappings. The interpreter executes the child synchronously, then
  resumes the parent with a synthetic PID. No host child process, Linux kernel,
  JIT or VM is used. Parents cannot run concurrently with children.
- There are at most 16 children over the entire invocation, with one child level.
  Nested clone returns EAGAIN. Thread/shared-memory flags, alternate stacks and
  TID/TLS arguments return EOPNOTSUPP. A child cannot create grandchildren even
  after exec. Consequently launching another shell as a child does not enable
  nested external commands.
- Parent and children share the original instruction budget and the existing
  file-write/create counters. At most 16 successful execs are allowed across
  the invocation. Exec and clone never replenish these bounds. Runtime faults or
  instruction exhaustion abort the entire invocation with status 125; they are
  not translated into simulated child signals. Existing bounded blocking I/O
  remains; this milestone does not introduce a total wall-time deadline.
- Regular-file descriptors have separate tables but share host open-file offsets,
  as with fork. Closing a child descriptor leaves its parent's descriptor valid.
  Clone with open directory or socket descriptors returns EOPNOTSUPP. Child-opened
  descriptors are closed on child completion. Standard output/error are inherited;
  stdin, descriptor duplication/redirection and pipes remain unsupported.
- `execve` opens a regular executable through the existing root-relative no-follow
  traversal. It supports the existing ARM64 static ET_EXEC format only. It stages
  and validates the new image before replacing the old image, returning ENOEXEC
  on invalid ELF without destroying the caller. Missing files, denied traversal,
  bad pointers and oversize arguments return guest errors. No host executable is
  launched and no shebang or dynamic loader is supported.
- argv must contain 1–64 entries; envp supports 0–64 entries. Strings share a
  64 KiB budget. Guest pointers are checked before image replacement. Successful
  exec resets CPU, heap and mapping state and preserves regular descriptors except
  those marked close-on-exec. The filesystem now tracks guest O_CLOEXEC and
  F_GETFD/F_SETFD independently of host descriptor protection. Exec with sockets
  open is rejected.
- `wait4` accepts a positive child PID or -1, options 0 or WNOHANG, an optional
  writable four-byte status pointer, and null rusage. Children have already
  completed when clone returns, so WNOHANG returns a completed child immediately.
  Normal exit is packed as `(code & 255) << 8`. Invalid status pointers do not
  consume the result; a second wait returns ECHILD. getpid/getppid expose synthetic
  identities (parent 1, children 2–17), never host identities.

These are intentionally restricted process semantics. Memory copies are bounded
by the existing address-space limits and single child level; this is not a new
production isolation boundary or a comprehensive aggregate memory quota.

### Shell contract

`/bin/sh -c COMMAND` accepts up to 4,096 input bytes, 32 parsed commands and 32
arguments per command. It parses the whole input before executing anything.
Whitespace, single/double quotes, backslash escapes, `;`, `&&` and `||` are supported.
Builtins are `true`, `false`, `exit [0–255]` and `status` (prints the last status).
The final command's status becomes the shell exit status; `exit` without an
argument uses the previous status. Missing commands return 127 and exec failures
return 126; syntax errors return 2. Exhausting the child limit returns 125.

Absolute executable paths are used directly; other names are prefixed with
`/bin/`. There is no PATH search. Use `busybox echo ...`, `busybox cat ...`, etc.
The fixture's `probe` command exists for validation of statuses and syscalls.

Expansion (including `$?`), globbing, substitutions, redirects, pipelines,
background jobs, scripts from files/stdin, terminal control and interactive input
are outside this milestone. Unsupported unquoted syntax is rejected; quoting and
escaping provide literal arguments within the documented small parser.

### Build and evidence

Guest rebuilding requires Clang's AArch64 target and LLVM ld.lld (the script can
also locate the linker bundled with Rust). The host runtime needs only its usual
C11 compiler. No guest compiler is needed to run the bundled fixtures.

```sh
python3 scripts/build_m19.py --cc /path/to/clang --ld /path/to/ld.lld
python3 scripts/build_native.py --sanitize
python3 scripts/verify_m19_native.py --sanitize
python3 scripts/verify_m19.py --native build/dockiva-runtime-sanitize
```

The manifest under examples/m19 hashes the shell, probe, startup, syscall helper,
guest ELFs and reused BusyBox ELF. The OCI fixture includes BusyBox; its existing
GPL source/license bundle and provenance remain under third_party/busybox and
examples/m18.

Mac validation is recorded in `mac-m19-validation.json`. Sanitizer and earlier
milestone regression reports are under `evidence/m19-*.json`. Linux/QEMU reference
comparison and upstream shell compatibility have not been run for M19. Pipes and
interactive terminals remain separate steps.

<a id="m20"></a>

## M20: bounded pipes and descriptor redirection

M20 extends the small guest shell with pipelines and file/descriptor redirection.
Pipeline stages still execute synchronously inside the interpreter. This supports
bounded command output, not concurrent streaming or an interactive terminal.
The M19 fixtures remain unchanged; use the new M20 image below.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m20_native.py
python3 scripts/verify_m20.py --lifecycle --report mac-m20-validation.json

python3 oci.py run examples/m20/shell-image -- -c 'busybox echo hello | busybox cat'
python3 oci.py run examples/m20/shell-image --writable -- -c 'busybox echo alpha >/out; busybox echo beta >>/out; busybox cat </out'
python3 oci.py run examples/m20/shell-image --writable -- -c 'probe both >/combined 2>&1; busybox cat /combined'
```

`oci.py run --writable` per mits writes only in that invocation's temporary guest
root, which is discarded after exit. Without it, OCI runs remain read-only.
Managed containers retain their existing private writable roots across starts.
There is no new host bind-mount or filesystem escape mechanism.

### Shell behavior and limits

- `|` connects one command's stdout to the next command's stdin. Each producer
  finishes before its consumer starts. At most **1 MiB** can be buffered per pipe.
  Commands that need concurrent consumers or produce more buffered output require
  a later scheduler/streaming milestone.
- A pipeline stops at the first stage that returns a nonzero status and uses that
  status. If every stage succeeds, it returns zero. This differs from the POSIX
  convention of reporting the final stage's status and avoids hiding an upstream
  buffer-capacity failure behind a successful consumer. `&&` and `||` apply to
  the whole pipeline.
- `< file`, `> file`, and `>> file` read, truncate/create, and append/create.
  Optional descriptor prefixes are 0, 1 or 2. `2>file`, `2>>file`, `2>&1`, `1>&2`,
  `<&0`, and `>&-` are examples. Other numeric descriptor prefixes are rejected.
- Redirections apply from left to right after pipeline endpoints are attached.
  `probe both >/out 2>&1` puts both streams in the file; `probe both 2>&1 >/out`
  leaves stderr on the original stdout. Explicit file redirection can override a
  pipeline endpoint.
- Quoting and escapes protect literal operators and filenames with spaces. The
  entire command is parsed before execution, so malformed syntax cannot partially
  execute earlier commands. Runtime redirection failures return 1; earlier valid
  redirections in the same command can already have created/truncated files.
- Builtins used in pipelines or with redirections run in children. `exit` in a
  pipeline exits only that stage; standalone `exit`, including with redirection,
  exits the shell. `status` prints and retains the previous status, as in M19.
- Guest stdin defaults to EOF. The host's interactive input is not exposed.
  Standard descriptors are restored when a child finishes; redirects do not leak
  into subsequent commands or change the runtime's own diagnostic streams.
- Existing limits remain: 4,096 command-text bytes, 32 parsed commands, 32 arguments
  per command, 16 children and 16 successful execs per invocation, one child level,
  and a shared instruction budget. Each command accepts at most eight redirects.

No background jobs, command substitution, expansion, globbing, here-documents,
PTYs, terminal control, interactive mode, BusyBox ash or POSIX shell compatibility
is claimed. M19's `true`, `false`, `status`, and `exit` builtins remain available;
external bare executable names resolve under `/bin/`.

### Runtime descriptor contract

The new adapter implements ARM64 `pipe2`, `dup`, `dup3`, and bounded
`fcntl(F_DUPFD/F_DUPFD_CLOEXEC)` allocation. Standard descriptors 0–2 and extended
slots 96–127 belong to this adapter; the existing socket and filesystem descriptor
ranges retain their previous numbering. Duplication targets are limited to 0–2
and 96–127; allocating duplicates starts at 96. Duplicating sockets or directories
is rejected. These are scoped descriptor operations, not a fully unified Linux
file table.

`pipe2` requires O_NONBLOCK and optionally O_CLOEXEC. Blocking mode is explicitly
rejected, since no concurrent process scheduler exists. There are at most eight
live pipes, each backed by a 1 MiB guest buffer. Clone shares these buffers and
retains endpoint references; close/exec/child completion release references.
The last endpoint close frees the buffer. Guest descriptor operations never close
or replace the host's standard descriptors.

Pipe reads return buffered bytes, EAGAIN when empty with writers present, and EOF
when empty after all writers close. Writes return EPIPE when no readers remain;
no guest SIGPIPE is delivered. A full buffer returns EAGAIN. Writes up to 4,096
bytes, including aggregate writev payloads, either fit completely or return
EAGAIN; larger writes may be partial. Reads/writes and writev totals are capped at
1 MiB per call, with at most 16 iovecs. Pipe fstat reports FIFO type; lseek returns
ESPIPE. F_GETFD/F_SETFD and F_GETFL are supported; changing pipe blocking mode,
pipe polling and readv remain outside this adapter.

Regular-file duplicates preserve shared offsets and pass through the existing
read-only policy and per-file/aggregate write limits. They support read, write,
writev, fstat, lseek and the documented fcntl operations. Other filesystem
operations on these aliases are not newly supported. Close-on-exec applies to
new descriptors and redirected standard descriptors; dup/dup3 without O_CLOEXEC
clears the new descriptor's close-on-exec flag. Failed duplication preserves its
target. Root-relative traversal, symlink rejection, executable validation and
M19's process restrictions remain active. This milestone does not close the
production isolation gate.

### Build and validation

Guest builds use Clang's AArch64 target and LLVM ld.lld, as in M19. Mac users can
run the bundled ELFs without a guest compiler:

```sh
python3 scripts/build_m20.py --cc /path/to/clang --ld /path/to/ld.lld
python3 scripts/build_native.py --sanitize
python3 scripts/verify_m20_native.py --sanitize
python3 scripts/verify_m20.py --native build/dockiva-runtime-sanitize
```

The build manifest hashes the guest sources and ELFs plus the reused M18 BusyBox
ELF. BusyBox's existing source/license bundle remains under third_party/busybox.
`mac-m20-validation.json` records Mac command and container acceptance;
`evidence/m20-sanitizer.json` records ASan/UBSan execution checks. Direct native
checks cover buffer and descriptor exhaustion, EOF/EAGAIN/EPIPE, writev atomicity,
failed duplication, clone/exec ownership, reclamation, and redirected-file quotas.
Regression reports for M7–19 are saved under `evidence/m20-regression-*.json`.
No independent Linux/QEMU M20 comparison or production isolation audit was run.

<a id="m21"></a>

## M21: concurrent pipeline streaming and backpressure

M21 adds opt-in scheduling for guest processes. Pipeline stages can now make
progress together, so a stream can exceed the pipe buffer's 1 MiB capacity.
Guest children remain inside the C interpreter; no Linux VM or host process is
created for each guest child. M19/M20 fixtures retain their default synchronous
behavior when `--schedule` is omitted.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m21_native.py
python3 scripts/verify_m21.py --lifecycle --report mac-m21-validation.json

python3 oci.py run examples/m21/shell-image --schedule --max-steps 100000000 -- -c 'busybox echo hello | busybox cat'
python3 oci.py run examples/m21/shell-image --schedule --max-steps 100000000 --writable -- -c 'probe large | busybox cat >/out; busybox ls /out'
```

`probe large` emits 4 MiB of `Z` bytes. To display the complete stream, run
`probe large | busybox cat`; tests compare all output bytes rather than only the
length. `probe slowcat` deliberately spends guest instructions between reads to
exercise producer backpressure.

`--max-steps` sets the invocation's shared instruction budget. OCI's default
remains 100,000, which is insufficient for many larger pipelines. The examples
explicitly request a larger finite budget. `--writable` retains M20's behavior:
writes affect a temporary private root and are discarded when an OCI run exits.

For a persistent managed container:

```sh
python3 containers.py create stream examples/m21/shell-image --schedule --max-steps 100000000 --arg=-c --arg='probe large | busybox cat >>/out'
python3 containers.py start stream
python3 containers.py inspect stream
```

The scheduler and budget settings are saved in that container's configuration.
Each start appends to its private `/out`, subject to the existing file-size and
write limits. Existing container configurations default to synchronous execution
and their original 10,000,000-step budget. Docker/Compose APIs have not gained a
new scheduling option in this milestone.

### Scheduling and process limits

- The interpreter uses round-robin slices of up to 4,096 guest instructions.
  Every process consumes the same global step budget, including syscall retries.
  A CPU loop cannot monopolize guest execution or acquire a fresh budget by exec.
- Up to four children may be live concurrently, plus the root process. The existing
  lifetime limits of sixteen children and sixteen successful execs remain. Nested
  child creation, threads and shared-address-space clone variants are rejected.
  The M21 shell rejects pipelines with more than four stages before execution.
- Scheduling saves CPU registers, flags, stack/ELF/heap/mapping state, TLS and SIMD
  registers, filesystem descriptors and enumeration state, and redirected/pipe
  descriptors. Lookup caches and exclusive reservations are invalidated when a
  context is restored. Pipe buffers and regular-file offsets remain shared through
  their existing descriptor ownership rules.
- Blocking pipe read/write/writev and blocking wait4 suspend the calling guest.
  It is retried after pipe or child state changes. Runnable guests continue using
  the same host interpreter thread; this is concurrency, not parallel host CPUs.
- wait4 with WNOHANG returns zero while a matching child is still live, and a
  completed child PID/status when available. Blocking waits retain the guest's
  arguments across suspension; errors do not consume a completed child's status.
- Exiting children release their memory and descriptors immediately and leave a
  waitable status. Root exit cancels remaining guest children and releases their
  resources; detached/background processes are not supported.
- If every remaining process is blocked and no recorded state change can wake one,
  the invocation fails with status 125 and a deadlock diagnostic. Budget exhaustion
  and interpreter faults also fail the whole invocation with status 125.

Address-space, descriptor and pipe limits remain bounded. With multiple live
images, memory consumption is higher than M20; this is not a new aggregate-memory
quota or production isolation guarantee. Socket creation is rejected in scheduled
mode. Pipe polling, general asynchronous host I/O, host-stdout backpressure,
threads and guest signal delivery remain separate work.

### Streaming and status behavior

With `--schedule`, pipe2 accepts blocking pipes as well as O_NONBLOCK pipes.
Buffers remain capped at 1 MiB, with at most eight live pipes. Empty blocking
reads suspend while writers remain; full blocking writes suspend while readers
remain. Readers receive EOF after the last writer closes and buffered data drains.
Writers receive EPIPE when all readers close; there is still no simulated SIGPIPE.
Nonblocking endpoints retain M20's EAGAIN behavior.

F_GETFL/F_SETFL can query/change a pipe endpoint's O_NONBLOCK state in scheduled
mode. Duplicated/inherited references share that endpoint setting. Small writes
and aggregate writev payloads up to 4,096 bytes retain the all-or-wait behavior;
larger writes may return partial progress. No bytes are replayed when a syscall
suspends after zero progress. M20's per-call I/O limits remain unchanged.

The M21 shell launches every stage, closes the parent's unused endpoints, and
waits for every stage. Redirections still apply left to right after pipe setup.
The pipeline returns the first nonzero stage status in command order, or zero if
all stages succeed. **A failed stage does not prevent already-launched stages
from running**, unlike M20's sequential stop-at-failure behavior. This retains
visibility of producer failures without claiming POSIX final-stage status rules.

Quoting, `<`, `>`, `>>`, standard-descriptor duplication, and M20's parser and
builtin limits remain. Subshells, nested external shells, interactive input,
terminals, job control and general POSIX/BusyBox ash compatibility remain open.

### Build and evidence

```sh
python3 scripts/build_m21.py --cc /path/to/clang --ld /path/to/ld.lld
python3 scripts/build_native.py --sanitize
python3 scripts/verify_m21_native.py --sanitize
python3 scripts/verify_m21.py --native build/dockiva-runtime-sanitize
```

Guest sources, ELFs and the reused BusyBox ELF are hashed in the M21 manifest.
The existing BusyBox source/license bundle is unchanged. Mac command/container
results are in `mac-m21-validation.json`; sanitizer results and legacy-mode
regressions are under `evidence/m21-*.json`.

The native scheduler test transfers 4 MiB through three stages with a slow middle
stage, checks every byte, asserts actual reader/writer suspensions and context
switches, and verifies task/pipe/file resource release. Its output is recorded in
`evidence/m21-scheduler-native.txt`. Command tests also cover four-stage streaming,
early reader exit, exec recovery, WNOHANG, live/lifetime child limits, root-exit
cleanup, CPU-budget exhaustion, deadlock detection, redirection and persistence.
No independent Linux/QEMU M21 reference, performance comparison or production
isolation audit has been performed.

<a id="m22"></a>

## M22 — scheduled guest cancellation

Implementation tested on Apple Silicon macOS. Full M22 acceptance remains open:
the production application corpus is not yet pinned. An independent ARM64
Linux comparison passed for default child termination, raw wait statuses and
reaped PID absence. This change is a bounded default-termination
implementation, not full POSIX signal support.

### Implemented behavior

With `--schedule`, ARM64 syscall 129 (`kill`) accepts positive synthetic guest
PIDs. Signal 0 probes existence; SIGINT (2), SIGKILL (9), SIGPIPE (13) and SIGTERM
(15) request default termination. Guest PIDs and signals never reach a host
`kill` API. Self-signalling stops execution before the next guest instruction.
Other tasks receive cancellation at their next scheduler selection, including
tasks suspended on pipe reads/writes or `wait4`.

Cancellation releases the task's memory and descriptors, updates pipe reference
counts and makes its raw signal number available in `wait4` status. The M22
shell reports `128 + signal`, including pipeline failures and conditionals.
Cancelling one child preserves its parent and siblings. Terminating the root
ends the invocation and releases its remaining children, as with normal root exit.

A zombie PID exists until reaped; signalling it succeeds without altering its
saved exit status. A reaped or unknown PID returns ESRCH. Invalid signal numbers
return EINVAL; unsupported signals, process groups and calls without `--schedule`
return EOPNOTSUPP. SIGKILL takes precedence over an earlier pending termination;
otherwise the first queued supported termination is retained.

### Reproduction

```sh
python3 scripts/build_native.py
python3 scripts/build_m22.py
python3 scripts/verify_m22.py --report mac-m22-validation.json
python3 scripts/verify_m22_native.py --sanitize
python3 oci.py run examples/m22/signal-image --schedule --max-steps 10000000 -- cancel-read
python3 oci.py run examples/m22/shell-image --schedule -- -c 'probe self-term; status'
```

The first OCI command prints `cancelled child; sibling and parent survived` and
exits 0. The shell command prints `143` and exits 143. Guest sources, ELFs and
the BusyBox fixture are hashed in `examples/m22/build-manifest.json`.

### Evidence and limits

The command verifier covers CPU-bound children, blocked pipe readers/writers,
root termination during a blocked wait, self-signals, zombie/reap behavior,
invalid/unsupported requests, scheduler-disabled behavior, shell pipelines,
conditionals and OCI execution. Cache-disabled variants exercise the same cases.
The direct native sanitizer verifier checks that blocked operations actually
suspended and all task, pipe and filesystem resources were released, including
root termination with a live child.

Reports: `mac-m22-validation.json`, `evidence/m22-sanitizer.json`,
`evidence/m22-native.txt`, `evidence/m22-linux-reference.json` and
`evidence/m22-regression-*.json`. There are 30 command checks in each of the normal
and sanitizer reports. Five direct sanitizer scenarios verify resource cleanup.

The Linux comparison uses the exact same portable probe ELF in the interpreter
and an existing ARM64 Linux image pinned by image ID, with networking disabled,
read-only mounts and bounded resources. `scripts/verify_m22_reference.py` records
the image/runtime/probe hashes and exact outputs. Docker is a test reference
only. Blocked-pipe scheduling and cancellation have local tests, not independent
Linux equivalence evidence; Linux pipe capacities and scheduling differ.

Existing bounds remain: four live children, sixteen lifetime children/execs,
single-level process relationships and one shared guest instruction budget.
No handlers, masks, alternate stacks, signal frames, stop/continue, process
groups, terminal Ctrl-C forwarding or host-to-guest signal bridge are implemented.
EPIPE still returns an error without automatically delivering SIGPIPE. Scheduled
networking remains unsupported. A passing cancellation suite does not establish
production isolation or support for arbitrary images.

The [corpus registry](ACCEPTANCE-CORPUS.json) pins current prototype fixtures and
lists the production targets still requiring exact artifacts and workload sizes.
The [execution ledger](MILESTONE-STATUS.md) retains the remaining M1–M80 gates,
including total RAM, CPU and battery comparison requirements.

<a id="m23"></a>

## M23 — foreground input and guest process groups

Implemented and tested on Apple Silicon macOS with real pseudo-terminals.
`--interactive` enables scheduling, reads the inherited standard input, and
forwards host Ctrl-C to the selected synthetic guest foreground group. The
default invocation still gives guests EOF on stdin.

The runtime never changes host terminal attributes, descriptor blocking flags,
or host foreground process groups. Canonical line editing and EOF therefore use
the host terminal's existing settings. Terminal input suspends a guest without
blocking runnable siblings. Waiting for human input does not consume guest
instructions. The instruction budget still bounds executing guests.

The opt-in group model supports getpgid/setpgid, kill of an explicit negative
group ID or the caller's group, TIOCGPGRP/TIOCSPGRP, and TIOCGWINSZ. Synthetic group
IDs never reach a host process-group API. After the foreground group exits, the
remaining parent can reclaim guest foreground ownership. SIGTERM to the host
runtime requests root termination and existing root-exit cleanup.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m23.py --report mac-m23-validation.json
python3 oci.py run examples/m23/signal-image --interactive --max-steps 10000000 -- echo-input
python3 oci.py run examples/m23/shell-image --interactive --max-steps 10000000 -- -c 'busybox cat'
```

Enter a line, then Ctrl-D on an empty line to finish, or Ctrl-C to terminate.
This is foreground input support, not an interactive command prompt.

Nineteen checks passed normally and under ASan/UBSan. They cover input/EOF,
Ctrl-C to the root, Ctrl-C to a child foreground group while the parent survives,
OCI forwarding, terminal preservation after success/signals/budget failure,
invalid ioctl pointers/groups, redirected stdin, cache-disabled group execution
and default EOF. Reports are `mac-m23-validation.json` and
`evidence/m23-sanitizer.json`; M21/M22 regressions also passed.

Limits: one guest session, no setsid, stop/resume, jobs/fg/bg, terminal mode
changes, background-read SIGTTIN or background-write SIGTTOU. Background reads
fail with EIO. The group adapter is deliberately limited and is not full POSIX
job control. Nested process relationships, post-exec setpgid restrictions and
signal handlers require further work. M22's production-corpus acceptance remains
open; this local implementation does not close that prerequisite.

<a id="m24"></a>

## M24 — bounded scripts with unmodified upstream ash

The executable is copied verbatim from the public Linux ARM64 BusyBox 1.37.0
musl image. Its image digest, platform image ID and ELF hash are recorded in
`examples/m24/build-manifest.json`. It is not the reduced M18 BusyBox build.
Docker extracts the test fixture and provides optional Linux references; the
runtime itself executes the ELF without a VM fallback.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m24.py --report mac-m24-validation.json
python3 oci.py run examples/m24/shell-image --process-tree --max-steps 10000000 -- -c 'echo "$(echo upstream | cat)"'
python3 oci.py run examples/m24/shell-image --process-tree --writable --max-steps 10000000 -- -c 'echo alpha >/out; echo beta >>/out; cat </out'
```

The new `--process-tree` option enables scheduled nested processes, parent-specific
waits and reparenting orphaned children to the guest root. Four live children and
sixteen lifetime children/execs still bound the invocation. Earlier `--schedule`
mode retains the single-level process restriction.

Implemented support includes static PIE load bias with rejection of PT_INTERP
and DT_NEEDED dependencies, additional integer SIMD operations and register
loads/stores, synthetic UID/GID and uname, default/ignored signal dispositions,
signal masks and pending default termination. Static PIE startup performs its
own relocations in guest code. External library loading is still unsupported.
Unsupported sendfile returns ENOSYS so BusyBox cat can use read/write.

The script corpus covers variables, arithmetic, functions, for/while/case,
conditionals, printf, external execution, nested shells, pipelines, substitution,
redirection, stderr and exit status. Signal/process probes cover masked delivery,
ignored pending signals, inherited dispositions, nested waits and orphan reaping.
An independent assembly probe exercises SIMD output byte-for-byte. Malformed
ELF checks cover load bias, entry, dynamic table and external-loader rejection.

Eighty-nine local checks passed normally and under ASan/UBSan. Twenty-six extra
Linux comparisons passed using the exact pinned image and probe ELFs, including
all script cases, SIMD output, signal masks and process waits. Reports are
`mac-m24-validation.json`, `evidence/m24-sanitizer.json` and
`evidence/m24-linux-reference.json`. Source hashes identify each tested revision.

Excluded: caught signal handlers and traps, job control, chdir, unlimited trees,
general BusyBox applet compatibility, dynamic libraries and full filesystem
semantics. `rt_sigaction` returns EOPNOTSUPP for caught handlers; upstream ash can
ignore that error, so a script using `trap` must not be treated as supported.
M25 and later compatibility work must address these gaps. Production corpus
selection and independent confinement qualification remain open.

The OCI wrapper now owns terminal Ctrl-C forwarding and uses a dedicated host
notification signal to the interactive runtime. This avoids delivering a single
terminal Ctrl-C twice when both wrapper and child share the host foreground
group. Repeated PTY regressions verify that a child cancellation leaves the
guest parent alive.

<a id="m25"></a>

## M25 — bounded ash job control and caught signals

`--job-control` enables interactive input, process trees, caught guest signals,
stop/resume and foreground-group ownership. It uses the pinned upstream M24 ash:

```sh
python3 scripts/build_native.py
python3 scripts/verify_m25.py --report mac-m25-validation.json
python3 oci.py run examples/m24/shell-image --job-control --max-steps 100000000 -- -i
```

Run `cat`, press Ctrl-Z, inspect `jobs`, try `bg`, then `fg`. Background terminal
reads stop the guest group with SIGTTIN. Ctrl-C terminates the foreground job and
leaves the shell usable. `exit` closes the session. The existing four-live-child
and sixteen-lifetime-child/exec limits still apply; this is a bounded session.

Guest signal frames preserve general registers, NZCV flags, masks and SIMD
registers; `rt_sigreturn` validates the frame before restoring guest state.
Malformed frames terminate the offending guest with SIGSEGV, preserving its
parent. Default/ignored dispositions, caught handlers with explicit restorers,
SA_SIGINFO, SA_RESTART, SA_NODEFER and SA_RESETHAND have scoped implementations.
Interrupted pipe reads return EINTR or restart according to SA_RESTART. Exec
resets caught handlers. Child notifications, stopped/continued wait statuses,
orphan reparenting and foreground process-group changes are integrated.

Frame offsets follow the Linux ARM64
[sigcontext ABI](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/include/uapi/asm/sigcontext.h)
and [signal entry/return implementation](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c).
The independent assembly/C probe validates register modification through ucontext,
SIMD restoration, bad-frame isolation, stop/continue and blocked-read handling.

The verifier exercises actual upstream ash traps and a real PTY session covering
jobs/bg/fg, Ctrl-Z, background-read stop, foreground resume and Ctrl-C. Host
terminal attributes remain unchanged after exit. Separate Linux comparisons run
the exact probe ELF in the pinned reference image; reports record actual outcomes.

Limits: no alternate signal stack, SVE, floating-point status changes, realtime
signal queues, full session management, full termios mutation or general terminal
emulation. Host canonical settings remain in force; upstream line editing can
echo input twice. No raw-mode/full-screen program support is claimed. Unsupported
signal flags/operations return explicit errors. M37 must broaden and qualify the
signal ABI alongside threads. Production security acceptance remains open.

<a id="m26"></a>

## M26: scheduled networking foundation

`--scheduled-network` enables cooperative loopback socket waits in the native
runtime, OCI runner and managed-container configuration. A blocking accept,
connect, read, write or socket poll suspends only its guest task. The scheduler
checks readiness and monotonic deadlines while CPU and pipe tasks continue.
Guest nonblocking flags are independent of the permanently nonblocking host
socket. Signals cancel blocked guests and cleanup releases their sockets.

Validation: `mac-m26-validation.json` records 29 checks including cache variants,
OCI execution, managed restart/log retention/removal, and five ARM64 Linux
comparisons. `evidence/m26-sanitizer.json` covers 19 command/OCI checks.
`evidence/m26-native.txt` records five sanitizer probes that additionally verify
actual suspension and descriptor/task/pipe cleanup. The existing M11–M17 suite
passed 30 checks with scheduled networking enabled, including concurrent HTTP,
slow peers, disconnects, lifecycle, registry, Docker API and Compose subsets
(`evidence/m26-scheduled-http-regression.json`).

Scope remains IPv4 TCP loopback. Fork/exec with live sockets, mixed pipe/socket
polling, DNS, outbound access and general Docker networks remain unsupported.
Timeouts are bounded to 30 seconds; a zero socket timeout uses that upper bound.
This is a scheduler foundation, not production network compatibility.

<a id="m27"></a>

## M27: filesystem compatibility (in progress)

Working directories now belong to guest processes. `chdir`, `fchdir`, `getcwd`, relative
open/stat/rename/exec and clone inheritance use directory descriptors without
changing the host process working directory. Children can change directories
without affecting their parent. Forked regular descriptors retain shared file
offsets. Forked directory descriptors share a cursor with reference-counted stream ownership; closing the child descriptor preserves the parent cursor. Exec preserves the working directory.

The root-relative, no-link policy remains in force, including rejection of `..`.
After a cwd is renamed, relative operations continue through its descriptor;
`getcwd` returns ENOENT until a verified pathname is established by another
chdir. It does not return a stale or host pathname. This differs from Linux's
ability to discover the renamed path and is an explicit remaining gap.

`mac-m27-validation.json` and `evidence/m27-sanitizer.json` cover 19 local checks each, plus a Linux comparison for fchdir/shared-directory lifetime:
three execution modes across cache variants, pointer/buffer errors, child cwd
isolation, shared offsets, symlink/traversal rejection, renamed directories,
and unmodified upstream ash using cd/subshells/relative files.

M27 remains open for directory dup/dup2 across descriptor classes, permitted link
semantics, physical parent traversal, fuller metadata and adversarial filesystem race qualification.

<a id="m28"></a>

## M28: bounded ARM64 musl dynamic loading

`--dynamic` enables PT_INTERP loading from the guest root, AT_BASE/AT_ENTRY/AT_PHDR
startup values, private file-backed guest mappings, fixed mappings confined to
the guest mmap arena, and mprotect. Host memory is never made executable.
Writable/executable guest mappings and ELF segments are rejected. Mapping errors
are checked before replacing old contents. Segment protection changes invalidate
the lookup cache. Exec failures preserve the calling image.

The pinned upstream fixture is Alpine 3.22.1 ARM64, digest
`sha256:4bcff63911fcb4448bd4fdacec207030997caf25e9bea4045fa6c8c44de311d1`.
BusyBox and musl bytes are unmodified. Library aliases are materialized as regular
files because the root adapter still rejects links. This is not full Alpine
image compatibility. `scripts/build_m28.py` records image and file hashes.

Validation: 26 checks in `mac-m28-validation.json`, including five Linux
comparisons; 21 local sanitizer checks in `evidence/m28-sanitizer.json`; mapping
bounds/protection/atomic-failure assertions in `scripts/verify_m28_native.py`.
The separate DSO probe exercises actual constructors, symbol relocation and TLS.
The Linux reference uses the kernel's normal executable/PT_INTERP entry path.
Standalone loader invocation of that probe is not in the validated contract
(the initial Linux reference attempt exited with SIGBUS). BusyBox shell scripts,
nested exec, pipelines, relative files and invalid interpreter rejection pass.
The 89-check static-shell suite also passes after this work.

Limits: 16 MiB ELF segment budget, 16 MiB mmap arena, 128 segments, private mappings
only, no general overlapping-load-segment support, no shared mappings, no general
SIGBUS delivery for accesses beyond a mapped file's EOF, and no glibc/JIT target.
Fixed mappings outside the mmap arena are rejected (including musl's optional
brk guard request). Broader loader and malformed-image qualification remains
open; these results do not imply arbitrary dynamic image support.

<a id="m29"></a>

## M29: first practical application candidate

The candidate is a bounded HTTP artifact fetcher using unmodified Alpine 3.22.1
BusyBox wget and musl, packaged in `examples/m29/fetch-image`. It downloads from
IPv4 loopback HTTP to stdout or a writable guest file. Configuration uses normal
wget arguments. DNS, HTTPS, proxies, external networks and the full Alpine image
remain outside this candidate's contract. No runtime VM fallback is used.

Implementation adds validated vectored reads and guest ITIMER_REAL/SIGALRM
handling. Alarms belong to guest tasks, use monotonic deadlines, are not inherited
by fork, and preserve the existing signal-frame mechanism. Timers have millisecond
resolution and a one-day bound; CPU timer kinds are explicitly unsupported. The scheduler
checks them while tasks are blocked. Managed configuration can now retain dynamic
loading, scheduled networking and job-control/signal flags across restarts.

`mac-m29-validation.json` and `evidence/m29-sanitizer.json` cover normal and slow
bodies, chunking, redirects, 404, truncated responses, a one-second timeout,
graceful guest SIGTERM, forced host termination, OCI stdout, two managed starts
with persisted output, and removal. Files survive restart in private container
state. A killed download is not promised to be atomically committed.

This is a scoped application candidate. M27's remaining filesystem gaps and
M31's confinement/aggregate-limit review remain prerequisites for acceptance.

The timer probe adds five local/sanitizer checks and four Linux comparisons for
invalid inputs (including timer installation before an old-value copy fault),
fork inheritance, periodic handlers, default SIGALRM status and prompt host
termination while a guest is blocked with a long alarm.

The Linux timer reference uses `docker --init` so the probe is an ordinary process.
Linux namespace-init default-signal immunity is not implemented for the runtime's
synthetic root PID; that remains a container-semantics compatibility gap.

<a id="m30"></a>

## M30: differential compatibility harness

`python3 scripts/verify_m30.py --linux-reference --report mac-m30-validation.json`
replays the versioned corpus against the interpreter and pinned ARM64 Alpine.
Each backend gets a fresh filesystem. Reports include runtime/source/corpus hashes,
actual exit status/stdout/stderr, the reference image identity and classification.

Eight cases currently match Linux. Two filesystem differences are explicitly
recorded: physical parent traversal and symlink following remain denied. Five
saved malformed/resource cases verify bounded rejection. Every unexpected runtime
or reference outcome fails the run; declaring a policy gap does not suppress
changes to either expected outcome. Four unit tests exercise the classifier.
Runs without Linux are labeled local-only and are not differential evidence.

The malformed corpus pins source hashes and exact mutation recipes. The workflow
in `.github/workflows/compatibility.yml` builds with sanitizers and replays that
corpus on Linux. Its configuration is present; no hosted CI execution has been
observed in this local workspace. Local and sanitizer evidence is recorded
separately. Larger application, concurrency and fuzz corpora remain future work.

<a id="m31"></a>

## M31: confinement and aggregate limits (in progress)

The interpreter now budgets all allocations made through its allocator across
all guest tasks. The default is 256 MiB, configurable with `--memory-limit-mib`
(1–1024). Counters include allocation headers and report current/peak bytes and
denials with `--profile`. These are allocation counters, not RSS or RAM savings.
A failed clone rolls back staged images and duplicated descriptors, returning
ENOMEM without corrupting the parent or leaking pipe references.

The managed state store serializes start admission and reserves each active
runtime's configured allocation budget. It admits at most four runtimes and
1024 MiB of reservations. Reservations follow supervisor-held locks and release
on exit. Other state stores, direct native invocations, libc allocations and
supervisor/OS memory are outside this accounting. Existing host CPU, descriptor,
core-file and file-size limits remain in the managed launcher.

Guest roots are scanned through descriptors before execution, with 128 MiB of
regular-file contents, 8192 entries and depth 32 bounds. Write/truncate growth
shares a counter across guest tasks. Deletion/shrink does not reclaim accounting
until the next run, which is conservative. Restart rescans persisted contents.
This does not impose an aggregate disk quota across all state stores.

On macOS, `--host-sandbox` adds an experimental default-deny Seatbelt policy via
`confined_exec.py --seatbelt`. It allows the explicit executable/guest root,
required system libraries, standard I/O and outbound loopback TCP. Root writes
require `--writable`. Explicit bind/listen operations and host fork/other exec are
denied. Nonstandard inherited descriptors are closed before sandbox entry.
This is a **client-only qualification candidate**, not the server sandbox.
The attempted localhost listener filter admitted wildcard listeners; that design
was rejected and all explicit binds were removed from the candidate policy.

The host-syscall test bypasses all guest adapters and checks outside-file read/write,
symlink/hardlink escapes, root write policy, outbound connections, explicit binds,
fork and exec. Both read-only and writable probes pass on this Mac, and the M29
HTTP/error/timeout/shutdown/OCI/managed-restart suite passes under the sandbox.
The installed macOS dyld-support profile informed startup directory permissions;
Apple identifies that interface as private and subject to change. See also
[Chromium's Seatbelt documentation](https://chromium.googlesource.com/chromium/src/+/main/sandbox/mac/)
for the distinction between policy testing and independent review.

Evidence: `mac-m31-validation.json`, `evidence/m31-sanitizer.json`,
`mac-m31-sandbox-validation.json`, `evidence/m31-sandbox-fetch.json`,
`tests/test_m31_capacity.py`, and `scripts/verify_m31_native.py`.

M31 remains OPEN: independent review, server-capable host confinement, cross-store
aggregate limits, filesystem races, broader OS/hardware qualification and abuse
campaigns are not complete. The opt-in policy does not make legacy/default runs
safe for arbitrary untrusted images. M33 public preview remains blocked.

<a id="m32"></a>

## M32: representative performance feasibility (in progress)

`scripts/benchmark_m32.py` measures the pinned dynamic shell startup and a verified
1 MiB HTTP fetch. Each run records wall time, user/system CPU time and the native
process's peak RSS via wait4, with fresh private filesystems and a bounded deadline.
It preserves raw samples and records the runtime hash. The local fixture server
is outside the measured process. The default run collected eight repetitions of
each workload in `evidence/m32-standalone-feasibility.json`.

These are engineering measurements for two small supported workloads. They do
not include managed supervisors, Docker Desktop's VM, whole-system idle cost,
energy or battery. No savings percentage is computed. Existing OrbStack timing
results remain separate from a future Docker Desktop comparison.

M32 remains OPEN: representative application stacks, agreed numerical budgets,
matched Docker Desktop measurements, full product/VM accounting, uncertainty
analysis and architecture feasibility review are not complete. RAM, CPU and
battery comparison remains part of final product qualification, as requested.

<a id="m35"></a>

## M35: instruction compatibility work in progress

The serialized interpreter now handles LDAR/STLR for byte, halfword, word and
64-bit accesses, additional loader SIMD operations, and architectural hints when
their optional features are absent. No optional CPU capabilities are advertised.
This is prerequisite work, not acceptance of M35 or shared-memory threading.

The added SIMD subset includes UZP1/2, integer MOVI with 16/32-bit lanes, scalar FMOV,
full-register and selected lane LD1/ST1, SIMD register-offset loads/stores,
CMEQ-zero, unsigned CMHI/CMHS, logical register/immediate operations, SHRN/SHRN2, ADDHN/ADDHN2 and vector ADDP.
Exclusive loads/stores also cover all four integer widths.
Reserved forms remain rejected. Guest memory permissions and stack alignment
checks still apply, including to vector accesses.

`examples/m35/acquire_release.S` checks results independently of the C decoder.
`scripts/verify_m35_acquire_release.py --linux` compares successful execution on
ARM64 Linux with normal and address/undefined-sanitized interpreter builds, with
both caches independently disabled. `verify_decode_cache.py` checks classification
and cache behavior; it does not independently establish instruction semantics.

M34 general shared-thread lifetime, concurrent reservation invalidation, adversarial
interleavings and M35 stress qualification remain open. The interpreter executes
one guest instruction stream at a time; these tests do not establish correctness
for a future parallel or translated backend.

Architecture references: [Arm instruction set reference](https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab),
[Arm memory ordering discussion](https://developer.arm.com/community/arm-community-blogs/b/tools-software-ides-blog/posts/armv8-sequential-consistency),
and [Arm engineer's GCS/CHKFEAT description](https://people.debian.org/~ema/minidebconf-Cambridge-2024-Slides/Capper.pdf).

Database startup follow-up adds unsigned vector multiply/widening addition,
scalar ADDP, CNT, ADDV, EXT, general-register lane insertion, raw S/W and D/X
transfers, and ADC/SBC flag handling. All remain subject to the scoped fixture
and Linux-reference checks; this is not a claim of complete ARM64 execution.

Further database prerequisites include vector lane INS/UMOV, multiple-structure
loads/stores with de-interleaving and post-indexing, EXTR, signed/unsigned widening
and fixed-width shifts, scalar DUP, UMINP, MOVI byte masks, signed pair loads and
non-temporal integer pairs. Prefetch hints do not access data in this interpreter.
Independent result tests cover these paths on Linux and normal/sanitized VM-free.
Scalar floating-point division remains unsupported and blocks Redis/Valkey.

<a id="m39"></a>

## M39: glibc startup work in progress

The direct ELF loader accepts opt-in input bounds of 16–64 MiB and mmap arenas of
16–128 MiB through `--elf-limit-mib` and `--mmap-limit-mib`. Defaults remain 16 MiB.
ELF load segments and individual guest lookups retain their prior bounds. All
allocations remain charged to the aggregate allocator limit. Input allocation
now uses the actual regular-file length plus one byte to detect length changes.

Startup supplies a bounded auxiliary vector with synthetic identity, zero optional
CPU capability masks, executable name and 16 fresh host-generated random bytes.
The vector has space for maximum argument/environment counts. It does not expose
host pointers. `verify_m39_native.py --sanitize` checks its layout and lifetime.

Descriptor-based `fstatat(AT_EMPTY_PATH)` supports glibc library metadata queries.
Historical MAP_DENYWRITE/MAP_EXECUTABLE flags are ignored, while mapping bounds,
private-only mappings and W^X policy remain enforced. Optional set_robust_list and
rseq registration, madvise and prctl explicitly return ENOSYS; no robust mutex or rseq support is
claimed. The read-only prlimit64 stack query reports the real fixed 1 MiB guest
stack. Changes to limits and other resources remain unsupported.

See the database probe report for actual application outcomes. M39 remains open:
threading prerequisites, locale/NSS, full images and application behavior have not
passed qualification. These native-only options are not enabled in Dockiva.

Database startup follow-up implements realtime/monotonic clock queries and
resolution, gettimeofday with a UTC guest kernel timezone, a read-only affinity
mask for the single guest CPU, and bounded getrandom reads (at most 256 bytes per
call, ordinary and NONBLOCK modes). Darwin uses its kernel CSPRNG; Linux uses
getrandom directly. Other random modes and clock IDs are rejected. Native
sanitizer checks cover output bounds, partial-length behavior, error paths and
fresh output; they do not certify a cryptographic construction.

`--uid`/`--gid` select one mapped identity (default 0). Get-ID syscalls, stat
ownership and auxiliary-vector identity agree. Reasserting the current identity
with setuid/setgid succeeds; changing identity is rejected. Host credentials never
change, and multiple guest owners/users are not implemented. Per-task umask,
regular-file/directory creation modes and chmod are covered by M44 native checks.
The process launcher now supports scheduled vfork with a shared image and parent
suspension until exec/exit; this does not implement general pthreads.

<a id="m44"></a>

## M44: filesystem prerequisites in progress

The filesystem adapter implements fsync and fdatasync on owned guest file and
directory descriptors. Both call host fsync; fdatasync may perform the stronger
metadata flush. This does not promise host power-loss durability or implement
Darwin F_FULLFSYNC. Crash ordering and database recovery are not yet qualified.

Bounded faccessat checks use the synthetic root identity and retain the read-only
and no-link policies. readlinkat returns only the final link's raw text; neither
ancestor links nor the returned target are followed. fstatat supports owned
file descriptors with AT_EMPTY_PATH. This does not add general symlink traversal.

`python3 scripts/verify_m44_native.py --sanitize` tests sync, close/reopen contents,
invalid descriptors, access modes, empty-path metadata, raw link reads and parent
traversal rejection. Atomic replacement, broader permissions, locking, shared
mappings, crash injection and persistence ordering remain open for M44–M48.
