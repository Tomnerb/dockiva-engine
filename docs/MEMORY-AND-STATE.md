# Memory and CPU state ownership

Ownership rules for address spaces, memory backings and CPU state, and the private-fork copy-on-write design.

- [Address-space state and backend synchronization — 2026-09-22](#address-space-ownership)
- [Reference-counted memory backing — 2026-09-22](#memory-backing-ownership)
- [First CPU-state ownership extraction — 2026-09-22](#cpu-state-ownership)
- [Private interpreter fork COW — 2026-09-22](#private-fork-cow)

<a id="address-space-ownership"></a>

## Address-space state and backend synchronization — 2026-09-22

The next part of [architecture slice A](TARGET-ARCHITECTURE-CONTRACTS.md) is
implemented. CPU and address-space state now have separate explicit structures
inside `RuntimeContext`. This is an extraction of existing bounded memory
behavior, not acceptance of general shared memory, COW or a complete process model.

### Ownership and execution contract

[GuestAddressSpace](../native/address_space.h) groups ELF/stack segments, loader
metadata, input bytes, brk backing and extent, mmap backing and page permissions,
mapping capacity, lookup-cache entries and a revision. [RuntimeContext](../native/runtime_context.h)
owns the active CPU and memory structures. Existing instruction, loader, syscall
and snapshot paths reference these structures through transitional active pointers.
Descriptor and scheduler state still prevent general reentrant/parallel use.

`ProcessImage` now snapshots the address-space structure instead of separately
copying a list of memory globals. These are borrowed snapshots: existing fork
paths explicitly duplicate backing; vfork uses the existing shared-image lifetime.
They are not independently owning deep copies. Cleanup releases segments, input,
heap and mmap backing together, clears lookup caches and preserves configured
mapping capacity. A restored snapshot receives a new worker-wide monotonic
revision; reusing an older snapshot cannot recreate a previously cached revision.

Successful mmap, munmap, brk and mprotect changes, loader/stack mapping additions,
snapshot restoration and reset notify the memory service. The interpreter drops
its segment lookup cache on notification. Unicorn compares the active owner and
revision before guest execution; if either changed, it invalidates old translated
blocks, unmaps its old view and maps the current backing/protections. Only after
successful synchronization does it record the new revision and resume execution.
Failure terminates the worker before it can run with a partially synchronized view.

This replaces the previous syscall-number list that triggered Unicorn remapping.
Unchanged mappings avoid a rebuild. The current synchronization still rebuilds
the complete backend mapping view when dirty; incremental updates are future work.
Interpreter and Unicorn views reference the same backing allocations, rather than
separate full copies of guest RAM. Backing may be released while guest execution
is stopped; the stale backend view must be synchronized before execution resumes.

Memory allocation and invalid-range failures tested here leave prior mappings,
contents and revision intact. Existing restrictions, including the dynamic
backend's rejection of writable executable pages, remain in effect. This does not
implement arbitrary self-modifying code or host-parallel memory coherence.

### Validation

The new [address-space harness](../tests/native/address_space_host.c), run with
[verify_address_space.py](../scripts/verify_address_space.py), exercises actual
interpreter instructions, mapping functions and Unicorn translations:

- Two address spaces use identical guest addresses with distinct ELF, heap and
  mmap backing. One hundred alternating interpreter/snapshot cycles preserve
  the expected values, code and fresh revisions.
- Allocation failure during fixed remap and an invalid mprotect range preserve
  the original mapping; permission revocation, unmap and remap change access and
  clear remapped data as expected.
- Twenty alternating Unicorn executions select the correct address space.
  Interpreter and Unicorn writes are visible through the same backing.
- Replacing code at an already translated guest address executes the new
  instruction. Revoking execute permission produces a protection fault, and
  unmapping produces an unmapped-fetch fault.
- Teardown releases all test backing under allocation accounting. The interpreter
  harness passes AddressSanitizer and UndefinedBehaviorSanitizer; the Unicorn
  harness runs signed with the required JIT entitlement.

| Regression gate | Result |
|---|---|
| Previous CPU ownership harness | Sanitizer and Unicorn variants passed |
| Backend ELF checks | 14 passed |
| Thread/futex checks | Three Unicorn and two ARM64 Linux reference checks passed |
| Interpreter shell/process corpus | 89 passed |
| Interpreter dynamic-loader corpus | 21 passed |
| Python tests | 63 passed |
| Prepared Redis queries/restart | Both cycles passed |
| Eight-client Redis recovery | Three clean cycles, two forced-crash cycles and final recovery passed; 651 acknowledged keys retained |
| Rebuilt helper through Dockiva | Four VM-free adapter tests passed, including startup and Redis Stop/Start/Restart persistence; one unrelated external-engine test explicitly skipped |

The rebuilt helper lives in `build/dockiva-address-space/vmfree-engine`. Tests used
isolated temporary state. The already open Dockiva application was not replaced;
native UI clicks were not tested. No RAM/CPU/battery improvement is claimed.

Evidence: [aggregate report and source/artifact hashes](../evidence/address-space/report.json),
[new memory tests](../evidence/address-space/memory-tests.json),
[CPU tests](../evidence/address-space/cpu-state.json),
[backend](../evidence/address-space/backend.json),
[Linux thread comparisons](../evidence/address-space/threads.json),
[Redis recovery](../evidence/address-space/redis-recovery.json),
[Dockiva adapter](../evidence/address-space/dockiva-tests.txt).

### Remaining work

Introduce explicit reference-counted backing objects and sharing rules before
implementing COW and general MAP_SHARED/fork behavior in the database backend.
The current interpreter fork remains an eager copy, and Unicorn still lacks
general process creation. FD ownership, scheduler unification and the complete
backend execution/fault interface also remain open. Preserve current regression
gates while progressing toward Valkey V0–V11 and PostgreSQL P0–P12; neither workload
matrix nor architecture slice A is accepted by this result.

<a id="memory-backing-ownership"></a>

## Reference-counted memory backing — 2026-09-22

The next part of [architecture slice A](TARGET-ARCHITECTURE-CONTRACTS.md) is
implemented: guest memory allocations now have explicit reference counts, and
segment mappings describe views into those allocations. This prepares ownership
for COW; private fork still makes eager copies. General Unicorn fork and Linux
MAP_SHARED remain unsupported.

### Ownership and behavior

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

### Verification

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

### Remaining work

Implement private fork COW with correct guest and host write handling and backend
invalidation, then extend process/shared-object semantics in the database backend.
FD ownership, scheduler unification and the full backend interface remain open.
The [Valkey V0–V11 and PostgreSQL P0–P12 gates](WORKLOAD-GATES-V1.md) remain open;
prepared Redis is regression evidence only. Slice A and M54 are not yet accepted.

<a id="cpu-state-ownership"></a>

## First CPU-state ownership extraction — 2026-09-22

The first part of architecture slice A is implemented. It gives CPU state an
explicit owner and uses the same representation in interpreter process snapshots
and the Unicorn register-transfer boundary. It does not complete the full
[state/backend architecture contract](TARGET-ARCHITECTURE-CONTRACTS.md).

### Changes and ownership

[GuestCpuState](../native/cpu_state.h) now contains general registers, PC/SP, TLS,
condition flags, SIMD registers, FPCR/FPSR storage and the interpreter's exclusive
reservation metadata. A `RuntimeContext` owns the active CPU state. Existing
instruction and syscall code accesses it through a transitional active CPU pointer;
the runtime remains cooperative and is not reentrant or safe for parallel host
execution merely because these fields have moved.

| Previous storage | Owner/boundary now |
|---|---|
| `regs`, `pc`, `sp`, condition flags | `GuestCpuState` |
| `thread_pointer`, `vectors` | CPU TLS and SIMD fields |
| Interpreter exclusive-address/size/valid globals | CPU reservation fields; validity still cleared on process restore |
| Separate CPU fields in `ProcessImage` | One CPU snapshot copied by save/restore |
| Unicorn scalar-only transfer with implicit SIMD/FP residency | Explicit `uc_transfer_cpu(engine, state, direction)` including SIMD and FPCR/FPSR, using batch register APIs |

Unicorn's opaque per-thread contexts continue to preserve backend-private state,
including its own exclusive monitor. This change does not replace Unicorn's
thread scheduler. FPCR/FPSR storage does not add unsupported interpreter floating
point instructions. The old trace record's `pc` field was renamed `address`;
diagnostic output is unchanged.

Address-space, descriptor and scheduler ownership remain to be extracted. The
full common execution/yield/fault interface is still pending. No new Linux
syscall, fork, shared-memory or application support is claimed.

### Validation

The [CPU-state harness](../tests/native/cpu_state_host.c) alternates 100 executions
between two distinct CPU owners using real interpreter instructions and process
snapshots. It checks that the inactive owner is unchanged, that registers,
TLS/SIMD/FP state survive restoration, and that teardown clears state and releases
the test allocation. This runs under AddressSanitizer and UndefinedBehaviorSanitizer.
A signed Unicorn harness additionally alternates 100 register transfers, checks
special registers through independent Unicorn IDs, and checks engine-side changes
are visible on readback. Both passed.

| Regression gate | Result |
|---|---|
| Experimental backend ELF checks | 14 passed |
| Thread/futex probes | Three Unicorn checks and two ARM64 Linux reference checks passed |
| Interpreter upstream shell/process corpus | 89 checks passed |
| Interpreter dynamic-loader corpus | 21 checks passed |
| Python unit tests | 63 passed |
| Prepared Redis query and clean restart | Both cycles passed |
| Redis eight-client contention and recovery | Three clean cycles, two forced-crash cycles and final recovery passed; 651 acknowledged keys retained |
| Rebuilt bundled engine through Dockiva adapter | Four tests passed, including automatic engine startup and Redis Stop/Start/Restart persistence; unrelated external live-engine test explicitly skipped |

Two alternating before/after query-and-restart timing runs took 3.52/2.80 seconds
before and 2.85/2.92 seconds after. This small diagnostic does not establish a
performance improvement or qualify overhead, memory, CPU or battery use.

The freshly packaged helper is at `build/dockiva-state-ownership/vmfree-engine`.
The existing open Dockiva test application was not replaced. The packaged adapter
check uses the new helper in isolated temporary state; native GUI clicks were not
part of this validation.

Evidence: [aggregate report and artifact/source hashes](../evidence/state-ownership/report.json),
[CPU harness](../evidence/state-ownership/cpu-state.json),
[backend](../evidence/state-ownership/backend.json),
[threads and Linux comparisons](../evidence/state-ownership/threads.json),
[Redis recovery](../evidence/state-ownership/redis-recovery.json),
[Dockiva adapter](../evidence/state-ownership/dockiva-tests.txt),
[diagnostic timing](../evidence/state-ownership/timing/comparison.json).
Run the new harness with `python3 scripts/verify_cpu_state.py` after building the
local Unicorn dependency. It signs its macOS Unicorn test binary for JIT use.

### Next implementation

Extract the current address-space state and ownership of its backing allocations,
then connect mapping/protection changes to backend invalidation explicitly.
Preserve the current CPU, interpreter and Redis gates. This remains slice A;
general process/shared-memory implementation follows in slice B. Slice C uses the
frozen baseline's Valkey V0–V11 gate first, then PostgreSQL P0–P12, retaining the
existing Redis tests throughout.

<a id="private-fork-cow"></a>

## Private interpreter fork COW — 2026-09-22

Historical checkpoint: [UNICORN.md#page-cow-unicorn-fork](UNICORN.md#page-cow-unicorn-fork) supersedes the whole-backing/macOS and Unicorn-fork limitations below.

Private fork in the interpreter now retains guest memory backing and copies it
when writable access is requested. This implements the next step after
[reference-counted backing](#memory-backing-ownership). Copying is at
backing-allocation granularity: a segment allocation, the heap arena, or the mmap
arena. It is not page-granular COW, and general Unicorn fork remains unsupported.

### Implementation

[Private fork](../native/address_space_ops.h) stages retained references, preserves
segment offsets and rolls back if any retain fails. Read-only access leaves
backing shared. Parent and child revisions change so backend views and lookup
caches cannot silently retain an obsolete sharing state.

Before returning a writable pointer, the memory service compares the backing's
reference count with the references owned by the active address space. If another
owner exists, it copies that backing and moves every local view to the new bytes.
This preserves aliases from permission splits and keeps previously returned
writable pointers stable across later lookups in the same stopped execution turn.
The original backing stays alive for peers. Lookup caches are invalidated after
replacement.

Interpreter scalar/vector stores and syscall output buffers use this write
boundary. Heap growth, anonymous mapping initialization and private file/fixed
mapping replacement also detach before changing bytes. Protection changes,
unmapping and heap shrinking update private metadata without copying data.

Failed detachment leaves bytes, references and mapping metadata unchanged.
`mmap` returns ENOMEM and `brk` preserves its previous break. A guest store or
syscall output lookup that cannot allocate a private backing terminates the
bounded worker with exit 125 and `Copy-on-write allocation failed`; Linux OOM
signal/recovery semantics are not implemented. Writable validation may trigger a
copy before an actual write, so this is a conservative write-access barrier.

`vfork` retains its existing shared-image behavior: writes are visible to the
suspended parent while remaining private from other forked images. Existing
exec replacement and failed-exec restoration rules are preserved.

Unicorn writes bypass interpreter lookup. Until a backend COW fault path exists,
[backend synchronization](../native/unicorn_backend.h) materializes any writable
shared backing before mapping it into Unicorn. Read-only protections remain
intact, and protection/revision changes trigger resynchronization. This ensures
isolation for explicit context tests; it does not enable the rejected general
fork syscall in the Unicorn database backend or provide lazy COW there.

### Validation and observed allocation behavior

The [COW harness](../tests/native/cow_host.c), run by
[verify_cow.py](../scripts/verify_cow.py), covers:

- Two nested private forks with the allocation budget already fully consumed.
  Backing allocation use remains **25,182,304 bytes**, an increase of **zero**.
  This measures backing allocations in the fixture, not process RSS, complete
  fork cost or Docker savings; address-space metadata still has a cost.
- Parent, child and grandchild write isolation, including actual interpreted
  stores, split segment offsets and reuse of already-private backing.
- Heap shrink/regrowth, fixed mapping replacement and unmapped-range reuse,
  including rollback when detachment cannot allocate.
- `readv` input metadata and multiple output buffers in different views of the
  same backing, preserving both output pointers and peer contents.
- The real process clone/store/exit/wait path and returned child status.
- Deterministic exit 125 on an exhausted-budget COW store, under sanitizers.
- A previously mapped Unicorn parent followed by sharing and resynchronization,
  child execution of a store, and rejection of a store to a protected page.

The older direct mapping, lookup-cache and shared-image harnesses were updated
from their stale global-state interfaces to the explicit address-space API.
The shared-image harness now also checks a `vfork` group against a separate COW
peer.

| Check | Result |
| --- | --- |
| COW harness | Sanitizer and Unicorn variants pass; exhausted-budget stores fail as expected in both |
| Backing, address-space and CPU-state harnesses | All pass |
| Direct mapping / lookup-cache checks | Pass; mapping check includes sanitizers |
| vfork shared-image / exec / wait | Sanitized host check, normal/sanitized interpreter and Linux reference pass |
| Python unit tests | 63 pass |
| Interpreter shell / dynamic loader | 89 / 21 pass |
| Unicorn backend / thread checks | 14 backend, 3 thread and 2 Linux-reference passes |
| Prepared Redis queries / recovery | 2 query cycles; 3 clean restarts, 2 forced crashes and final recovery; all 651 acknowledged keys retained |
| Rebuilt Dockiva helper adapter | 4 pass; 1 separate live-engine test skipped |

The rebuilt helper is `build/dockiva-cow/vmfree-engine`. The Dockiva adapter checks
its automatic startup, packaged shell/HTTP behavior and prepared Redis
PING/SET/GET, stop/start, persistence and restart. The installed Dockiva app was
not replaced, and GUI interaction was not tested.

Evidence: [aggregate results and hashes](../evidence/cow/report.json),
[COW harness](../evidence/cow/cow-tests.json),
[backing](../evidence/cow/backing-tests.json),
[address spaces](../evidence/cow/address-space.json),
[vfork and Linux comparison](../evidence/cow/vfork-tests.txt),
[Redis recovery](../evidence/cow/redis-recovery.json),
[Dockiva adapter](../evidence/cow/dockiva-tests.txt).

### Next work and limits

Move toward page-granular storage and backend write-fault handling, then general
process/shared-object ownership in the database backend. A first heap write still
copies its 8 MiB arena; an mmap write can copy the configured 16–128 MiB arena.
This step therefore does not establish low-memory database performance.

General MAP_SHARED, shared futex identity, unified FD/scheduler ownership and the
full backend interface remain open. Architecture slice A and M54 are not accepted.
[Valkey V0–V11 and PostgreSQL P0–P12](WORKLOAD-GATES-V1.md) remain open; prepared
Redis continues to serve as regression evidence only.
