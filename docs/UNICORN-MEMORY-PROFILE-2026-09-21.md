# Memory-path profiling — 2026-09-21

**A targeted experimental patch improved the memory loop by 4.71x.**
The previously observed roughly 61x native slowdown was not a performance ceiling.
The final matched comparison measured **59.3x native before, 12.6x after**.
Arithmetic remains roughly 1.85x native. No database or Dockiva performance claim
follows from these synthetic results.

## What changed

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

## Matched results

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

## Correctness evidence

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

## Decision

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
