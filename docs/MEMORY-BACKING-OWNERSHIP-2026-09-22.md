# Reference-counted memory backing — 2026-09-22

The next part of [architecture slice A](TARGET-ARCHITECTURE-CONTRACTS.md) is
implemented: guest memory allocations now have explicit reference counts, and
segment mappings describe views into those allocations. This prepares ownership
for COW; private fork still makes eager copies. General Unicorn fork and Linux
MAP_SHARED remain unsupported.

## Ownership and behavior

[MemoryBacking](../native/memory_backing.h) contains a reference count, allocation
size and aligned byte storage. [Backing operations](../native/memory_backing_ops.h)
allocate, retain, copy and release storage, checking allocation and reference-count
overflow. References are non-atomic because execution remains cooperative on one
host thread.

Each segment owns one backing reference and an offset. Heap and mmap arenas each
own one backing reference. Loader input remains a separate temporary allocation.
Borrowed process snapshots do not increment backing references; their existing
scheduler and `SharedImage` lifetime rules still determine the single owning
image. This change does not introduce a new process-sharing policy.

A segment `mprotect` split now retains before/middle/after views of the original
allocation. It changes protection metadata without allocating or copying the
segment bytes. Resource accounting charges an allocation once, including its
headers, regardless of the number of mapping references. This is internal
allocation accounting, not a host RSS or battery measurement.

[Address-space private copying](../native/address_space_ops.h) stages a complete
copy before committing it. It copies each segment's visible range and the heap
and mmap arenas. An allocation failure releases partial work and leaves both
source and destination unchanged. Last-reference teardown frees backing once.

Interpreter lookup and Unicorn mapping use the same backing bytes plus the
segment offset. Mapping revisions still trigger backend synchronization before
execution. No new Linux syscall, backend capability or workload gate is claimed.

## Verification

The new [sanitizer harness](../tests/native/memory_backing_host.c) checks:

- Splitting a three-page segment at a fully consumed allocation budget succeeds
  without copying bytes or charging the allocation again.
- Reference-count overflow rejects a split without mutating the mappings.
- Private copies preserve offset data and isolate segment, heap and mmap writes;
  a failure after the first copied segment cleans up partial allocations.
- A retained view survives its original address space's teardown, and the final
  release returns tracked allocation usage to zero.
- Startup argument, environment and random-data writes address backing bytes.

The startup test was added after regression testing caught two stale raw-buffer
accesses in this refactor; both accesses were fixed before final verification.

The [address-space harness](../tests/native/address_space_host.c) also checks
Unicorn execution through the first and last views of a split segment, while
execution of the protected middle page fails. Interpreter harnesses run under
AddressSanitizer and UndefinedBehaviorSanitizer.

Final checks:

| Check | Result |
| --- | --- |
| Memory backing sanitizer harness | Pass |
| Address-space and CPU-state harnesses | Sanitizer and Unicorn variants pass |
| Python unit tests | 63 pass |
| Interpreter shell regressions | 89 pass |
| Dynamic loader regressions | 21 pass |
| Unicorn backend checks | 14 pass |
| Thread/futex checks | 3 Unicorn and 2 Linux-reference passes |
| Prepared Redis queries | 2 cycles pass |
| Prepared Redis recovery, eight clients | 3 clean cycles, 2 forced crashes and final recovery pass; 651 acknowledged keys retained |
| Rebuilt Dockiva helper adapter | 4 pass; 1 separate live-engine test skipped |

The helper is built at `build/dockiva-memory-backing/vmfree-engine`. Adapter
checks cover its packaged shell/HTTP path and prepared Redis PING/SET/GET,
stop/start, persistence and restart. The existing installed Dockiva app was not
replaced, and GUI interaction was not tested.

Evidence: [aggregate report and hashes](../evidence/memory-backing/report.json),
[backing tests](../evidence/memory-backing/backing-tests.json),
[address-space tests](../evidence/memory-backing/address-space.json),
[CPU tests](../evidence/memory-backing/cpu-state.json),
[backend](../evidence/memory-backing/backend.json),
[thread/Linux checks](../evidence/memory-backing/threads.json),
[Redis recovery](../evidence/memory-backing/redis-recovery.json),
[Dockiva adapter](../evidence/memory-backing/dockiva-tests.txt).

## Remaining work

Implement private fork COW with correct guest and host write handling and backend
invalidation, then extend process/shared-object semantics in the database backend.
FD ownership, scheduler unification and the full backend interface remain open.
The [Valkey V0–V11 and PostgreSQL P0–P12 gates](WORKLOAD-GATES-V1.md) remain open;
prepared Redis is regression evidence only. Slice A and M54 are not yet accepted.
