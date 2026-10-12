# CPU adapter decision — 2026-09-21

**Conditional go for an isolated compatibility prototype. No-go for adopting
Unicorn as Dockiva's production/default engine or claiming an efficiency win.**
The tested CPU primitives and local hardened launch work. The memory-performance
concern remains. This batch does not make PostgreSQL, Redis or Valkey usable.

This is the second authorized experiment, capped at 10,000 tokens or 30 minutes.
It ran from 13:58:07 to 14:06:20 UTC (493.99 seconds). Work is confined to experiment sources, build artifacts
and evidence; all 14 native runtime source hashes still match the existing build
manifest. Dockiva was not modified or rebuilt.

Follow-up: [profiling found a 4.71x memory-loop improvement](UNICORN-MEMORY-PROFILE-2026-09-21.md). The measurements below are the earlier unpatched baseline.

## Measurements

Unicorn 2.1.4, static C linkage, Apple Silicon Mac with 32 GiB RAM. Same assembly
as the previous experiment: one million iterations, one discarded warmup and
five measured samples with alternating mode order. Every checksum matched native.
Engine construction and teardown are outside the timer; first translation and
the execution timeout mechanism are inside it. No instruction/memory hooks are
installed for these benchmarks. These are synthetic kernel timings, not database
throughput or Docker comparisons.

Medians from the locally signed hardened helper:

| Mode | Arithmetic | 64 KiB load/store loop |
| --- | ---: | ---: |
| Native ARM64 | 2.108 ms | 0.359 ms |
| Default TLB, normal mappings | 4.034 ms | 22.038 ms |
| Default TLB, host-backed mappings | 3.854 ms | 21.635 ms |
| Virtual TLB, normal mappings | 3.980 ms | 132.726 ms |
| Virtual TLB, host-backed mappings | 3.971 ms | 132.317 ms |

Default mode is **1.91x native for arithmetic and 61.39x for this memory loop**.
The ordinary helper measured 1.83x and 57.15x respectively. Host-backed mappings
did not materially resolve the slowdown. Virtual TLB was approximately six times
slower than default for the memory loop. Removing Python from the execution path
also did not resolve the previous approximately 61x result. These tests narrow
the problem but do not identify its precise internal cause; no profiler or
translator modification was attempted.

Upstream describes virtual TLB as a simplified mapping path with only a
theoretical performance advantage. Our result is evidence against using that
switch as the solution for this workload.
[Pinned Unicorn FAQ](https://raw.githubusercontent.com/unicorn-engine/unicorn/2.1.4/docs/FAQ.md)

## Functional checks

Both ordinary and hardened helpers passed all checks in both default and virtual
TLB modes:

- **SVC interception:** 100,000 simulated getpid calls returned the expected
  checksum; a separate unknown syscall returned negative ENOSYS. Hardened timings
  were 15.00–15.75 ms for the 100,000-call loop. This includes the loop and a small
  C callback; no filesystem, networking or Linux syscall implementation is timed.
- **Shared memory and saved CPU state:** two contexts alternated 1,000 times each
  in one engine. Their shared counter reached 2,000 while private registers,
  stack pointers and TLS pointers retained their expected values. This tests
  cooperative context switching, not parallel threads, futexes, atomic ordering,
  signals or complete floating-point state.
- **Invalid accesses:** ten cases per mode covered normal and host-backed memory,
  reads after unmapping, writes after removing write permission, reads after
  removing all permission, execution from non-executable data, and an unmapped
  address. Data access was first executed successfully to populate caches before
  permissions/mappings changed. Each fault matched its expected Unicorn error.
- **Time limit:** an infinite loop returned after approximately 5.03 ms with a
  5 ms timeout.
- **External stop:** a host thread requested cancellation after a block callback
  confirmed execution had begun. Hardened join latency was 22–32 microseconds;
  the fallback timeout was not used. These are individual observations, not
  stress-tested latency guarantees. Each complete helper also had a 30-second
  parent-process deadline.

The guest only receives mapped code and a 64 KiB data region. Mapping/fault checks
are not a security audit, a host-process sandbox, or a total allocator/RAM cap.

## macOS delivery check

The static helper ran successfully after ad-hoc signing with hardened runtime
and **only `com.apple.security.cs.allow-jit`**. Strict signature verification
passed, and inspection records the `adhoc,runtime` flags. No unsigned-executable-
memory or disabled-library-validation entitlement was added.

This proves local hardened execution on this Mac. Developer ID signing,
notarization, Gatekeeper, App Sandbox, Dockiva packaging and clean-machine/older
macOS launch remain untested. It does not explain the earlier tool-sandbox
failure. Apple's JIT guidance remains relevant to product packaging.
[Apple JIT documentation](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon)

## Decision and next scope

The CPU backend can support the tested adapter operations. That removes one
uncertainty, but leaves the Linux compatibility layer described in the
[first decision](FEASIBILITY-DECISION-2026-09-21.md): shared-VM threads, futex,
epoll, process ownership, IPC, filesystem/image semantics and persistence.
Unicorn does not supply those features for this project.

If continuing VM-free development, target **one isolated Redis service prototype**
with an explicit acceptance sequence: pinned ARM64 executable starts, a real
client performs PING/SET/GET, background threads operate, then AOF data survives
stop/restart. First scope its required Linux interfaces and execution model.
Do not treat another instruction patch, version output or these synthetic tests
as service completion. PostgreSQL and Valkey need their own later acceptance.
This is substantial compatibility work, not a promise to finish within another
small experiment. The proposed service scope has not been implemented here.

Do not reopen M34–M80 wholesale, ship this backend, or advertise lower RAM/CPU/
battery consumption based on this result. Licensing compatibility remains an
integration gate; the upstream GPLv2 core label is recorded in the earlier
report, but a product distribution decision was not made in this experiment.
No real-service performance, total RAM, battery or 8 GB hardware test was run.

## Reproduction and evidence

Run `python3 experiments/unicorn-adapter/run.py` from this repository on the same
Apple Silicon environment. It requires the existing isolated Unicorn 2.1.4
installation, clang, codesign and Homebrew LLVM objcopy. The local paths are
intentional for this experiment, not a portable build system. JIT execution may
require leaving the tool sandbox with authorization.

- [Harness](../experiments/unicorn-adapter/probe.c)
- [Build/run script](../experiments/unicorn-adapter/run.py)
- [Structured results, samples summary and hashes](../evidence/unicorn-adapter/report.json)
- [Hardened raw samples](../evidence/unicorn-adapter/hardened.jsonl)
- [Signature and entitlements](../evidence/unicorn-adapter/codesign.txt)
- [Runtime source verification](../evidence/unicorn-adapter/runtime-unchanged.json)

Earlier `probe.jsonl` records the initial five-fault-case run. `ordinary.jsonl`
and `hardened.jsonl` are the final expanded ten-case results. The helper only
contains synthetic guest code; it does not execute or modify database images.
