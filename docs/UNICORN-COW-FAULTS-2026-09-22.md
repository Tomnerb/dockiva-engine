# Unicorn COW write faults — 2026-09-22

Historical checkpoint: [PAGE-COW-UNICORN-FORK-2026-09-23.md](PAGE-COW-UNICORN-FORK-2026-09-23.md) supersedes the whole-backing/macOS and Unicorn-fork limitations below.

Unicorn now defers copies of shared private backing until a guest store, extending
[interpreter fork COW](PRIVATE-FORK-COW-2026-09-22.md). Backend entry no longer copies
all writable shared allocations. This provides the memory boundary needed for
future database-backend processes; the general Unicorn fork syscall remains
rejected. Copies still cover whole backing allocations, not individual pages.

## Fault and retry protocol

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

## Validation

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

## Python and remaining production work

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
