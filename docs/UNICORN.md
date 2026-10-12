# Unicorn adapter (removed)

Decision record and work on the Unicorn CPU adapter. The adapter has since been removed; the native Hypervisor.framework backend replaced it.

- [CPU adapter decision — 2026-09-21](#unicorn-adapter-decision)
- [Memory-path profiling — 2026-09-21](#unicorn-memory-profile)
- [Unicorn COW write faults — 2026-09-22](#unicorn-cow-faults)
- [macOS page COW and bounded Unicorn fork — 2026-09-23](#page-cow-unicorn-fork)

<a id="unicorn-adapter-decision"></a>

## CPU adapter decision — 2026-09-21

**Conditional go for an isolated compatibility prototype. No-go for adopting
Unicorn as Dockiva's production/default engine or claiming an efficiency win.**
The tested CPU primitives and local hardened launch work. The memory-performance
concern remains. This batch does not make PostgreSQL, Redis or Valkey usable.

This is the second authorized experiment, capped at 10,000 tokens or 30 minutes.
It ran from 13:58:07 to 14:06:20 UTC (493.99 seconds). Work is confined to experiment sources, build artifacts
and evidence; all 14 native runtime source hashes still match the existing build
manifest. Dockiva was not modified or rebuilt.

Follow-up: [profiling found a 4.71x memory-loop improvement](#unicorn-memory-profile). The measurements below are the earlier unpatched baseline.

### Measurements

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

### Functional checks

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

### macOS delivery check

The static helper ran successfully after ad-hoc signing with hardened runtime
and **only `com.apple.security.cs.allow-jit`**. Strict signature verification
passed, and inspection records the `adhoc,runtime` flags. No unsigned-executable-
memory or disabled-library-validation entitlement was added.

This proves local hardened execution on this Mac. Developer ID signing,
notarization, Gatekeeper, App Sandbox, Dockiva packaging and clean-machine/older
macOS launch remain untested. It does not explain the earlier tool-sandbox
failure. Apple's JIT guidance remains relevant to product packaging.
[Apple JIT documentation](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon)

### Decision and next scope

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

### Reproduction and evidence

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

<a id="unicorn-memory-profile"></a>

## Memory-path profiling — 2026-09-21

**A targeted experimental patch improved the memory loop by 4.71x.**
The previously observed roughly 61x native slowdown was not a performance ceiling.
The final matched comparison measured **59.3x native before, 12.6x after**.
Arithmetic remains roughly 1.85x native. No database or Dockiva performance claim
follows from these synthetic results.

### What changed

A five-second macOS `sample` capture of the owned helper found most guest execution
samples in the store helper and its memory-mapping/dirty-page handling. In one
main-thread execution subtree, 2,569 of 3,526 samples were under `store_helper`.
Sampling also captured launch and sleeping timeout threads; those are excluded
from this observation. JIT frames are partly unsymbolized.

Source inspection found `notdirty_write` keeps a page on the slow path when its
CPU TLB entry has an executable address. In this default ARM64 setup, that field
can be present even when the Unicorn mapping itself is non-executable data.
Consequently, ordinary data writes repeatedly traverse the helper.

The one-line candidate changes the executable-page guard to inspect the memory
region's `UC_PROT_EXEC` permission. The existing snapshot and memory-hook guards
remain. Executable mappings retain dirty-write handling for code invalidation.
This is a local patch to the downloaded Unicorn 2.1.4 experiment source; the
installed wheel and project runtime were not changed.

- [Exact patch](../experiments/unicorn-profile/nonexec-dirty.patch)
- [Pinned upstream function](https://github.com/unicorn-engine/unicorn/blob/2.1.4/qemu/accel/tcg/cputlb.c#L1189)
- [Before profile](../evidence/unicorn-profile/default.sample.txt)
- [After profile](../evidence/unicorn-profile/patched.sample.txt)

The after-profile no longer lists the store/notdirty helper hotspot; exit-request
checks are now prominent. We did not remove those checks or make a second
optimization. The experiment does not establish optimal performance or prove
this guard change correct for every architecture/MMU/hook combination.

### Matched results

Both libraries were built from the pinned 2.1.4 source using identical
RelWithDebInfo configuration, Apple clang and ARM64-only guest support. The
candidate source differs by the one-line patch. The source-built baseline
reproduces the earlier wheel's roughly 22 ms memory result.

Both comparison helpers were ad-hoc signed with hardened runtime and only the
allow-jit entitlement; strict signature verification passed. The sequence was
baseline, patched, patched, baseline. Each execution discarded one warmup and
measured five samples per workload/mode with alternating mode order: ten final
samples per build/workload/mode. Every checksum matched native. The outer deadline
was 30 seconds per helper; guest execution retained its timeout. Setup and
teardown are outside the timer; initial translation is included.

| Default TLB, ordinary mappings | Baseline median | Patched median | Change |
| --- | ---: | ---: | ---: |
| Arithmetic, 1M iterations | 3.8105 ms | 3.6945 ms | No meaningful targeted improvement claimed |
| Memory, 1M iterations | 22.5210 ms | 4.7835 ms | 4.71x faster; 78.8% less time |
| Native memory reference | 0.3795 ms | 0.3790 ms | Essentially unchanged |

Patched arithmetic / its native reference: 1.85x. Patched memory / its native
reference: 12.62x. Host-backed memory also improved, to 4.876 ms. Virtual TLB
remained slow, approximately 145 ms; this patch is not a solution for that mode.
No confidence interval or real-application speed prediction is claimed.

### Correctness evidence

Baseline and candidate passed:

- The previous adapter's checksum, syscall interception, shared-context, memory
  fault, timeout and external-cancellation checks in both TLB modes.
- All 15 applicable upstream ARM64 unit tests, including MMU and memory-hook tests.
- New targeted checks, also in both TLB modes and hardened helpers: installing a
  write hook after warming the fast path still observes all 100 writes; hook
  removal stops callbacks; remapping the same address read-only rejects stores;
  two CPU+memory snapshot restores undo guest stores; changing a warmed data page
  to executable and rewriting its code with a guest store executes the new code.

An initial blanket upstream CTest invocation also ran x86-dependent `test_mem`
and `test_ctl` against the ARM64-only baseline. They failed with unsupported
architecture and subsequent test crashes; their raw log is retained. They are
not passing coverage and were excluded from the final applicable suite. This
experiment does not qualify the common-code patch for x86 or other architectures.

The checks support retaining this candidate for further integration evaluation.
They do not constitute security qualification, an exhaustive MMU/snapshot/hook
test, or Developer ID/notarization/App Sandbox validation.

### Decision

**Accept the patch as an experimental performance improvement.** It demonstrates
that profiling can improve the measured memory path substantially. Do not install
it as Dockiva's default engine on this evidence alone. Upstream review or broader
backend qualification is needed before adopting a maintained fork.

The Linux thread, futex, event-loop, process, filesystem and persistence gaps
remain. PostgreSQL, Redis and Valkey readiness is unchanged. No RAM, battery,
Docker, database-throughput or 8 GB hardware benchmark was performed. All 14 native
runtime source hashes still match the pre-experiment build manifest.

Reproduction instructions and evidence:

- [Build instructions](../experiments/unicorn-profile/README.md)
- [Comparison and validation script](../experiments/unicorn-profile/validate.py)
- [All final samples and artifact hashes](../evidence/unicorn-profile/comparison.json)
- [Pinned source URL and archive SHA-256](../evidence/unicorn-profile/source.json)
- [Batch timing and decision](../evidence/unicorn-profile/decision.json)

<a id="unicorn-cow-faults"></a>

## Unicorn COW write faults — 2026-09-22

Historical checkpoint: [#page-cow-unicorn-fork](#page-cow-unicorn-fork) supersedes the whole-backing/macOS and Unicorn-fork limitations below.

Unicorn now defers copies of shared private backing until a guest store, extending
[interpreter fork COW](MEMORY-AND-STATE.md#private-fork-cow). Backend entry no longer copies
all writable shared allocations. This provides the memory boundary needed for
future database-backend processes; the general Unicorn fork syscall remains
rejected. Copies still cover whole backing allocations, not individual pages.

### Fault and retry protocol

The [backend adapter](../native/unicorn_backend.h) removes write permission from
its mapping when the guest mapping is writable but another owning address space
references its backing. Linux guest protections remain authoritative and unchanged.
Local permission-split views are counted together, so multiple views belonging
to the same address space do not spuriously trigger COW.

A write-protection hook records the fault address and size and returns failure.
The adapter waits until emulation has stopped before validating guest permissions,
detaching the affected backing through the memory service and resynchronizing the
backend. The hook does not replace pointers during an active translated store.
The faulting instruction then retries at its guest PC.

The resolver checks guest-page chunks when a store crosses adjacent mapping views.
Real read-only and unmapped writes remain errors. If the other owner has exited,
the retained backend COW protection is removed without an unnecessary allocation.
Revision changes invalidate old translations and mapping views before execution
resumes. Failed allocations terminate the worker with the existing explicit COW
failure, before writing shared bytes.

The faulted instruction's budget charge is refunded before retry, preserving the
existing instruction and scheduling-slice budgets. Retries are bounded by the
number of possible backing allocations; a backend that fails to make progress
cannot spin indefinitely. The adapter does not recreate the CPU context during memory synchronization;
exclusive stores may retry through the guest's ordinary reservation loop.

### Validation

[verify_unicorn_cow.py](../scripts/verify_unicorn_cow.py) runs a dedicated
[store/fault harness](../tests/native/unicorn_cow_host.c) against the actual linked
Unicorn backend. It verifies:

- Backend synchronization and guest reads allocate no private copies.
- Scalar, post-index, paired, SIMD, release and cross-page stores isolate peers.
  Only the backing touched by the store is copied; heap/mmap backing stays shared
  when a segment is written.
- The post-index base register advances once, and the four-instruction store/exit
  sequences consume exactly four instructions despite fault retries.
- Heap and mmap stores detach their respective backing.
- An exited peer leaves a sole owner that can write under a fully consumed runtime
  allocation budget without copying its original bytes.
- Genuine read-only and unmapped writes remain faults, while writes crossing
  adjacent writable segment views succeed.
- An exclusive load/store retry loop completes and preserves peer contents.
- The production execution loop reaches the expected exit status with a shared
  image supplied by the harness; an exhausted allocation budget fails with exit 125.

The revised [COW harness](../tests/native/cow_host.c) also exercises this path with
nested private images, guest permissions and an exact instruction budget. Its
interpreter variant runs with AddressSanitizer and UndefinedBehaviorSanitizer.

| Regression | Result |
| --- | --- |
| Dedicated Unicorn COW harness | Store matrix, allocation-failure case and production loop pass |
| Common COW harness | Sanitizer/Unicorn variants and both expected failure cases pass |
| Address-space / CPU-state harnesses | Sanitizer and Unicorn variants pass |
| vfork | Sanitized shared-image check, normal/sanitized interpreter and Linux reference pass |
| Python unit tests | 63 pass |
| Interpreter shell / dynamic loader | 89 / 21 pass |
| Unicorn backend / threads | 14 backend, 3 thread and 2 Linux-reference passes |
| Prepared Redis queries / recovery | 2 query cycles; 3 clean cycles, 2 forced crashes and final recovery preserve 651 acknowledged keys |
| Rebuilt Dockiva helper | 4 adapter tests pass; 1 separate live-engine test skipped |

The helper is built at `build/dockiva-unicorn-cow/vmfree-engine`. The existing
installed Dockiva app was not replaced, and GUI interaction was not tested.

Evidence: [aggregate report and hashes](../evidence/unicorn-cow/report.json),
[store/fault cases](../evidence/unicorn-cow/backend-cow-tests.json),
[common COW checks](../evidence/unicorn-cow/cow-tests.json),
[Linux vfork comparison](../evidence/unicorn-cow/vfork-tests.txt),
[Redis recovery](../evidence/unicorn-cow/redis-recovery.json),
[Dockiva adapter](../evidence/unicorn-cow/dockiva-tests.txt).

### Python and remaining production work

This change is entirely in C runtime/backend execution. The current Dockiva preview
still embeds CPython for its API, OCI preparation and supervisor. Packaging removes
the need for a system Python installation; it does not remove Python execution.
The [project definition](PROJECT-DEFINITION.md#removing-python-from-the-shipped-engine)
now makes that distinction explicit and records the Go dockivad replacement gate.
Python is allowed for development tools in the frozen production architecture,
but the shipped production engine must use Go dockivad and the C workers.

Page-granular COW, general Unicorn process creation, MAP_SHARED, unified
FD/scheduler ownership, Go control-plane migration and workload acceptance remain
open. The prepared Redis regression does not qualify Valkey or PostgreSQL. This
step closes neither architecture slice A nor M54, and establishes no Docker RAM,
CPU or battery saving.

<a id="page-cow-unicorn-fork"></a>

## macOS page COW and bounded Unicorn fork — 2026-09-23

The native runtime now supports private guest processes on Unicorn, including
fork from a multithreaded parent. Prepared Redis 7.4.2 passes background RDB
snapshot and AOF rewrite tests, with acknowledged parent writes while the child
is active and data validation after restart. This does not qualify stock images,
Valkey, PostgreSQL, M46, M54 or production Docker replacement.

### Memory behavior

On macOS, guest backing uses contiguous Mach VM allocations. Private fork still
retains backing without copying bytes. The first write by a different owner
creates a private Mach copy-remap view; subsequent writes copy host pages on
demand. Stable contiguous pointers preserve syscall spans and multiple `readv`
output buffers. Split segment views retain their offsets and local aliasing.

The COW unit on the tested Apple Silicon host is **16 KiB**, while guest mappings
and protections remain **4 KiB**. This is host-page COW, not 4 KiB physical COW.
Other hosts retain the existing portable whole-backing COW implementation.

The memory service reserves logical page charges before writable access. A failed
multi-page reservation leaves ownership and bytes unchanged. VM snapshots and
page metadata release their charges on final ownership release. An instruction
or child-TID store that cannot obtain a COW charge terminates the bounded worker
with status 125; child-TID failure teardown is also sanitizer-tested. These counters
are conservative allocation accounting, not RSS or physical-memory measurements.
For a 64 MiB backing, the allocation test observes zero added backing bytes at
fork, 49,240 tracked bytes for the first child write (view metadata plus one host
page), and 16,424 bytes for another host page. Rewriting a private page adds no
charge. This is not a Docker RAM/CPU/battery comparison.

Unicorn protects shared pages and resolves write faults after emulation stops.
A contiguous range with unchanged backing can update backend permissions
incrementally when the preceding memory revision matches. Missed revisions,
changed pointers, mapping changes and exec require full synchronization. Guest
read-only protection stays authoritative, including split views within one host
page. Retried stores retain exact instruction-budget accounting.

### Process and descriptor behavior

Each live guest process has its own Unicorn engine and translation cache. Threads
within that process share its memory and descriptor table. On a process switch,
a previously synchronized mapping view can be retained only when it matches the
saved process revision. A forked child inherits the caller's architectural CPU
state, including SIMD and TLS; it starts with a fresh exclusive monitor.

The shared process services provide private image snapshots, parent/PID state,
regular-file offsets, directory cursors, pipes, epoll ownership, exit status,
wait/reaping, reparenting and bounded termination. Socket open descriptions are
reference-counted: close in a child does not close the parent's socket or erase
its epoll registration. Socket status flags and timeouts are shared; CLOEXEC
remains descriptor-local. Futex waits are scoped to the owning address space.

Limits and explicit exclusions:

- One root plus at most four live children; sixteen child creations per run.
- Eight live guest tasks across all process/thread groups.
- A 16 MiB translation-code buffer per engine, checked against Unicorn's actual
  configured size: at most 80 MiB for five live engines. Metadata, allocator
  overhead and total RSS are not covered by this code-buffer bound.
- Private `clone(SIGCHLD)` supports optional parent/child TID stores and
  clear-child-TID. Existing supported thread-clone flags remain available.
- Nested private fork and fork from a multithreaded parent are tested. Only the
  calling thread appears in the child.
- Exec is supported for a single-threaded process without open sockets, including
  failed-exec rollback and fresh backend CPU state after successful exec. Unicorn vfork, arbitrary clone combinations, caught-signal
  delivery and complete Linux wait/exec semantics remain incomplete.
- MAP_SHARED, System V IPC, cross-process shared futexes and PostgreSQL shared
  state are still open. The runtime remains cooperative on one host thread.

No Redis-name syscall branch or host-service fallback was added. Native execution
and the Go/C Dockiva bundle contain no Python; Python is test/build tooling.

### Evidence and reproduction

The reports record executable hashes and exact scope:

- [COW allocation/isolation and sanitizer checks](../evidence/cow/cow-tests.json).
- [Unicorn store retries, cross-host-page SIMD and permission deltas](../evidence/unicorn-cow/backend-cow-tests.json).
- [Fork/FD/thread/exec probes](../evidence/unicorn-fork/probes.json) and
  [sanitized probes](../evidence/unicorn-fork/sanitizer-probes.json).
- [Prepared Redis background operations and restart validation](../evidence/unicorn-fork/redis-background.json).
- [Concurrent Redis/AOF process-crash recovery](../evidence/unicorn-fork/redis-recovery.json).
- [Packaged profile negotiation](../evidence/unicorn-fork/profiles.json),
  [Dockiva bundled adapter integration](../evidence/unicorn-fork/dockiva-integration.json)
  and [signed artifact provenance](../evidence/unicorn-fork/artifact.json).

The Redis test starts with 64 values of about 4 KiB, then inserts 16 keys and
updates 16 existing values after requesting background work. It requires observed
acknowledgements while the child is still active. RDB restart must restore original
values and exclude the new keys; AOF restart must restore all 80 keys with updates.
`SHUTDOWN NOSAVE` prevents the shutdown path from replacing the tested RDB.
Each background operation has a 20-second completion deadline and a 20-second
socket timeout; the existing concurrent recovery test retains its own bounds.
This is a short deterministic dataset, not a sustained-load or power-loss test.

During development, rebuilding all mappings on every process switch caused about
5-second replies and failed the parent-progress check. Preserving per-process
engines/caches reduced the observed reply time to about 0.22–0.24 seconds and
background completion to about 0.4 seconds before final bundle verification.
These are local diagnostic observations, not a native or Docker benchmark.

Build and run from the repository root:

```sh
python3 scripts/bundle_dockiva_engine.py --output build/dockiva-fork
python3 scripts/verify_unicorn_fork.py --runtime build/dockiva-fork/vmfree-engine/_internal/build/vmfree-c-unicorn
python3 scripts/verify_redis_fork.py --runtime build/dockiva-fork/vmfree-engine/_internal/build/vmfree-c-unicorn
```

The separate signed helper is `build/dockiva-fork/vmfree-engine/vmfree-engine`.
This checkpoint builds and verifies the engine bundle; it does not replace or
qualify the installed Dockiva desktop app. GUI packaging and clicks are separate
from the runtime evidence. The next compatibility work is shared memory/IPC and
signal semantics, with Valkey's pinned workload gates and PostgreSQL's process
workflows providing the acceptance tests.
