# Milestones with acceptance gates

0. Interpreter seed (this package): real ARM64 ELF, output and exit, bounded
   execution, rejection tests. Mac hello and 12 tests passed per user report.
1. Independent validation: run on an Apple Silicon Mac; assemble hello.S using an
   independent Linux ARM64 toolchain and compare stdout/status on real ARM64 Linux.
   Independent LLVM/Zig probes, Linux QEMU-user comparison and M1 Mac acceptance
   now pass (Mac results reported by user). Native ARM64 Linux hardware comparison
   remains pending.
   Preserve fixtures and results. Do not claim general compatibility from hello.
2. Execution backend spike: C interpreter, 20 checks and Python/C startup/branch
   benchmarks implemented. Mac compiled-backend acceptance passed per user report; its microbenchmark
   wall-time ratios were 14.04x startup and 61.56x branch dispatch versus Python. Translation
   experiments and the broader benchmark matrix below remain future work.
   Measure arithmetic, branches, syscall loops, startup, RSS and CPU against a
   comparable lightweight-VM workload on the same Mac. Record failures too.
3. Small static programs: M3 adds a freestanding C demo, integer/memory ISA
   extensions, writable guest memory and minimal Linux startup stack. Linux
   reference and local checks pass; M3 Mac acceptance passed per user report. Fuller startup/auxv and broader syscall coverage remain open; bounded heap
   support is implemented in M4 below.
4. Guest allocation: bounded brk, anonymous mmap and partial munmap implemented.
   17 M4 checks and Linux reference comparisons pass; M4 Mac acceptance passed per user report.
   General allocator/libc compatibility remains open.
5. One static HTTP server: M5 adds localhost blocking IPv4 sockets and a
   one-request freestanding C HTTP server. 14 checks pass locally, including
   Linux reference comparisons. M5 Mac acceptance passed per user report. Nonblocking event
   handling, concurrency and broader errno/signal behavior remain open. Avoid assuming Go
   binaries will be easy: their runtime needs threads, signals and other features.
6. Image integration: local OCI layers/config verification, bounded extraction and
   supported entrypoint execution implemented in M6; 24 checks pass on Linux.
   Mac acceptance passed per user report. Guest filesystem access is added below.
   No claim of arbitrary image compatibility.
7. Read-only filesystem: implemented in the combined M7–M10 release. The HTTP
   guest reads its HTML from the image through the root-relative adapter.
8. Writable state: full private image copies, file/directory mutation and restart
   persistence implemented. No copy-on-write optimization or aggregate disk quota.
9. Static libc compatibility: a normally linked musl POSIX C program passes
   startup, file-read, errno and argument checks, matching QEMU Linux. Compatibility
   is limited to the exercised instructions/syscalls; broader apps remain open.
10. Lifecycle: create/start/stop/list/inspect/logs/rm with one supervisor per
    running instance. Combined 40 checks pass on Linux and on the user's Mac per report.
11. Practical networking: implemented a scoped poll/nonblocking loopback TCP
    adapter and persistent eight-client HTTP demo. Linux acceptance passes;
    external networking, DNS and a fully asynchronous proxy remain unsupported.
12. Image distribution: local OCI Distribution pull/cache and ARM64 index selection
    implemented. Local HTTP fixtures pass; public registry/auth/CDN acceptance
    and mutable-tag updates remain pending.
13. Isolation review: resource limits, implementation review and rejection tests
    delivered. Independent audit, verified OS confinement, fuzz/race campaigns,
    aggregate resource quotas and untrusted-image acceptance remain OPEN.
14. Performance checkpoint: HTTP timing harness and same-workload Dockerfile
    delivered, with Linux runtime-only samples. Same-Mac VM comparison and
    CPU/memory accounting remain OPEN. No superiority claim.
15. Docker API subset: authenticated local endpoints implemented. Docker CLI
    27.5.1 version/images/create/start/ps/inspect/logs/stop/rm pass on Linux.
16. Compose subset: actual Docker Compose 2.32.4 up/ps/traffic/stop/restart/down
    passes for the supplied two-service host-loopback example. JSON subset runner
    included. Named volumes, DNS and general Compose semantics remain unsupported.
17. Mac distribution: packaging/signing/notary scripts, versioned installer and
    diagnostics delivered. Signed clean-Mac execution, Python bundling, automated
    updates and tested rollback remain OPEN.

M11–M17 is a combined release candidate, not seven fully closed acceptance gates.
See MILESTONES.md#m11-17 for exact implemented scope and external validation needs.
M0–M10 Mac acceptance passed per user report; this candidate's Mac checks are pending.

Dynamic linking, threads, signals, general subprocess semantics and further ISA coverage remain
separate compatibility workstreams against concrete target applications.
Go, Node.js and PostgreSQL require their own acceptance milestones.

Do not introduce a Linux VM fallback into the VM-free path silently. Full Docker
compatibility, production security and better performance remain goals to validate.

## M18 — selected static BusyBox commands

Implemented and validated locally: minimal unmodified-source BusyBox 1.37.0 echo, cat, pwd, ls, mkdir; required ARM64 forms and rooted filesystem adapters; OCI lifecycle. Mac validation pending. See [M18 notes](MILESTONES.md#m18). Shells, pipes and process creation remain separate future work.

## M19 — bounded processes and basic shell

Implemented: synchronous single-level guest clone, rooted static execve, wait4
exit statuses, shared instruction and process limits, descriptor inheritance and
close-on-exec, and a small noninteractive guest shell. Mac validation and sanitizer
checks cover command sequences, failures, memory isolation and bounds. See
[M19 notes](MILESTONES.md#m19). Upstream ash, concurrent children, pipes and
interactive terminals remain future work.

## M20 — bounded pipes and descriptor redirection

Implemented: nonblocking guest pipes with bounded shared buffers, descriptor
duplication and close-on-exec, sequential shell pipelines, and ordered file/stdout/
stderr redirection. A failed stage stops its pipeline. Temporary OCI runs can opt
into writable roots. Mac command, sanitizer and lifecycle checks cover buffering,
EOF, failures, redirection ordering and persistent container output. See
[M20 notes](MILESTONES.md#m20). Concurrent streaming, full descriptor-table
semantics and interactive terminals remain future work.

## M21 — concurrent pipeline streaming and backpressure

Implemented opt-in round-robin guest scheduling, up to four live children,
blocking pipe suspension/retry, wait4/WNOHANG, and streaming beyond the 1 MiB pipe
buffer capacity. The M21 shell launches all stages and reports the first nonzero
stage status after waiting for all of them. Mac validation covers 4 MiB streams,
backpressure, early exits, deadlock/budget bounds and container persistence. See
[M21 notes](MILESTONES.md#m21). Interactive terminals, nested processes, guest
full signal handling, scheduled networking and general asynchronous I/O remain future work.

## M22 — scheduled guest cancellation

Implemented guest-only kill for positive synthetic PIDs, existence probes,
default termination for SIGINT/SIGKILL/SIGPIPE/SIGTERM, blocked-task cancellation,
signal wait statuses and shell status translation. Mac and sanitizer tests cover
cleanup, unrelated-task survival and zombie/reap behavior. See
[M22 notes](MILESTONES.md#m22). Portable signal status comparisons pass on
ARM64 Linux; production corpus selection remains open. This is not full POSIX
signal compatibility.

## Proposed production program: M22–M80

[Production Docker-replacement roadmap](PRODUCTION-ROADMAP.md) defines every
proposed milestone from M22 through M80, dependencies, acceptance gates and release
criteria. The [status ledger](MILESTONE-STATUS.md) tracks implementation separately
from acceptance. M33 is a controlled preview; M63 an internal hardened runtime candidate;
M74 a replacement beta; M79 a release candidate; M80 production v1 for the published
Apple Silicon/Linux ARM64 workflow scope. Separate M81–M86 programs cover broader
architectures, platforms and orchestration. Numbers are planning IDs, not dates or
a guarantee that universal Docker compatibility is achievable.
