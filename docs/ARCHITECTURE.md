# Architecture and decision record

## Current implementation and adopted target — 2026-09-22

The user-supplied [final architecture v1.0](FINAL-ARCHITECTURE-V1.md) is the frozen
production baseline: SwiftUI/AppKit UI, Go dockivad, C Linux Personality/runtime,
no Python in production execution, ARM64 first, one shared guest-memory model
and bounded global/per-container resources. Valkey V11 and PostgreSQL P12 are the
major workload gates. The [component contracts](TARGET-ARCHITECTURE-CONTRACTS.md)
define subordinate ownership, interfaces, implementation order and
acceptance tests. Global resource management, isolation
and optimized backend components are explicitly distinguished from implemented
behavior. This design does not close a milestone acceptance gate.

Today Dockiva's adapter talks to [Go dockivad](DOCKIVA-INTEGRATION.md#dockivad-go-migration)
over an authenticated Unix socket. Go validates OCI images, manages private state
and supervises C guards/native runtime workers. The shipped path contains no Python.
Legacy Python CLIs remain development and migration-reference tools.
The interpreter remains the default; the separately packaged prepared Redis
preview selects the experimental Unicorn backend through a [digest-bound profile](DOCKIVA-INTEGRATION.md#compatibility-profiles). Native capability negotiation runs before launch; Inspect exposes the effective policy. Both paths use our Linux
compatibility services. The C runtime still has substantial global state and
separate interpreter/Unicorn scheduling paths; extracting shared state ownership
is the first implementation slice. Its [CPU-state portion](MEMORY-AND-STATE.md#cpu-state-ownership)
now has an explicit owner shared by interpreter snapshots and the Unicorn transfer
interface, with regression evidence.

[Address-space state and backend synchronization](MEMORY-AND-STATE.md#address-space-ownership)
have subsequently been extracted. Unicorn now synchronizes mapping revisions
before execution instead of relying on a syscall-number list.
[Reference-counted backing](MEMORY-AND-STATE.md#memory-backing-ownership) now owns segment,
heap and mmap allocations. Segment permission splits retain offset views without
copying data. [Private interpreter fork COW](MEMORY-AND-STATE.md#private-fork-cow) now
shares backing until writable access, with rollback on failed detachment.
[Unicorn COW fault handling](UNICORN.md#unicorn-cow-faults) now maps shared
writable backing with backend write protection and retries stores after private
copy/remapping. [macOS host-page COW and bounded private fork](UNICORN.md#page-cow-unicorn-fork) now
use separate Unicorn engines/caches per process, shared thread state within each
process, and reference-counted inherited sockets/pipes/epoll handles. Prepared
Redis background persistence and snapshot isolation pass. MAP_SHARED, complete
signal/exec behavior, unified FD ownership and scheduler extraction remain open.

Prepared Redis query, concurrency and recovery evidence is linked from the
[status ledger](MILESTONE-STATUS.md). The [Dockiva Redis test build](REDIS.md#dockiva-redis-preview)
passes its packaged adapter tests. PostgreSQL/Valkey service qualification,
machine-wide resource management and production isolation remain open.

## Historical decisions and implementation updates

The following entries describe their original milestone scope. Statements about
interpreter-only execution and language selection are historical, not a claim
that the experimental Unicorn backend or the adopted Go daemon direction is absent.

Goal: run useful, unmodified ARM64 Linux user programs on Apple Silicon macOS
without a Linux kernel VM, eventually offering a Docker-compatible interface.

Current flow: local OCI layout (optional) -> verified layers/config -> ELF bytes
-> validated guest segments -> ARM64 interpreter -> Linux syscall dispatcher
-> bounded memory operations or restricted host output/loopback sockets.

Guest virtual addresses are offsets into interpreter-owned segment data. They are
never used as host pointers. Guest SVC instructions are decoded as data; Linux
syscall numbers are never passed directly to the macOS kernel.

We start with interpretation because it makes syscall interception explicit and
is portable. This does not establish that transparent native execution or a fast
JIT is feasible on macOS. Same CPU architecture does not mean syscall compatibility.
Research native/JIT execution separately, including Apple code signing, executable
memory, exception behavior, and instruction-cache requirements. Do not depend on
disabling SIP or modifying host security controls for a normal product install.

Before production-language selection, compare a compiled interpreter and a small
translation prototype against this reference. Rust or C/C++ are candidates for the
execution core; Go is a candidate for a future daemon/API. M2 adds a C11 experiment; the production language is not selected.

Isolation must be designed separately. A root filesystem directory alone is not a
container boundary. Filesystem mediation, process semantics, network behavior,
resource controls, and host confinement all need explicit design and testing.

Primary references checked September 14, 2026:

- ARM ELF ABI: https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst
- Linux syscall definitions: https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h
- ARM A-profile architecture / ISA: https://developer.arm.com/Architectures/A-Profile%20Architecture

References are living documents. Pin revisions when expanding compatibility.

## M3 update

The C backend now has writable segments, a separately mapped 1 MiB stack, minimal
Linux startup vectors and additional integer/control/memory instructions. Python
remains the reference for the M0/M1 subset; M3 guest behavior is checked against
Linux QEMU user-mode using independently compiled ELFs. The C choice remains an
experimental implementation, not a final product-language decision.

## M4 update

The active C backend routes raw Linux allocation calls to native/heap.h. brk has
an 8 MiB cap and anonymous mappings use a separate 16 MiB arena with per-page
allocation/protection metadata. Partial unmapping clears accessibility metadata;
backing storage is reused and released on process exit. No host executable memory
or VM is involved. ELF/stack allocation and memory-lookup bounds remain in place.

## M5 update

The C backend maps supported Linux socket syscalls through native/net.h. Guest
descriptors index a bounded table of host sockets. Linux sockaddr bytes and errno
values are translated explicitly; macOS structures are not reused as guest ABI.
The guest server constructs HTTP responses itself and communicates over actual
loopback TCP. Binding, descriptor, buffer and timeout limits are prototype policy,
not a substitute for future isolation design.

## M6 update

The Python OCI adapter merges validated layers in memory, writes a fresh private
temporary tree, and launches the native interpreter with image argv/env. It
forwards stop signals and cleans up after child exit. The extracted tree supplies
the ELF; it does not implement a mediated guest filesystem. See [M6 in MILESTONES.md](MILESTONES.md#m6)
for accepted image types, limits and concurrency assumptions.

## Combined M7–M10 update

The native fs.h adapter opens ancestors relative to a guest root descriptor,
rejects links/traversal and translates ARM64 file flags and stat fields explicitly.
Files have a separate bounded descriptor table. Managed containers each hold a
full private rootfs copy; no shared writable image layer exists.

containers.py creates state, spawns an owning supervisor and exposes lifecycle
commands. The supervisor holds an exclusive lock, captures logs, records exit
status and accepts token-authenticated stop requests over loopback. It does not
use a stored PID as stop authority. Crash recovery and aggregate disk quotas are
not implemented. The execution root is a prototype policy boundary, not audited
production isolation against hostile images or same-user host tampering.

A normally linked musl static executable now exercises actual libc startup and
selected POSIX calls. New integer/vector operations and TPIDR_EL0 storage support
that target; this is not a general ARM64, musl or Linux conformance claim.

## M11–M17 candidate

The native socket adapter adds bounded ppoll/nonblocking handling and loopback
connect. registry.py obtains and verifies OCI data into an immutable-name cache.
daemon.py exposes an authenticated Engine API subset backed by existing private
container copies/supervisors. compose.py executes a restricted JSON Compose model
through that same API; actual Docker CLI/Compose clients are also tested.

Managed children run through confined_exec.py for core/file/descriptor/CPU limits.
This is resource hardening, not a proven OS isolation boundary. Default Mac API
transport is a private Unix socket; authenticated loopback TCP is available.
Per-container networking still shares host loopback and explicit ports.

Benchmark and Mac packaging tools are separate from runtime execution. No Linux
VM fallback exists. Security, paired Mac performance and signed clean-Mac install
acceptance remain open as documented in the candidate guide.
