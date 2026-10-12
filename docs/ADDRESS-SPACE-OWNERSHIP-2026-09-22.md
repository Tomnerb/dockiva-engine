# Address-space state and backend synchronization — 2026-09-22

The next part of [architecture slice A](TARGET-ARCHITECTURE-CONTRACTS.md) is
implemented. CPU and address-space state now have separate explicit structures
inside `RuntimeContext`. This is an extraction of existing bounded memory
behavior, not acceptance of general shared memory, COW or a complete process model.

## Ownership and execution contract

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

## Validation

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

## Remaining work

Introduce explicit reference-counted backing objects and sharing rules before
implementing COW and general MAP_SHARED/fork behavior in the database backend.
The current interpreter fork remains an eager copy, and Unicorn still lacks
general process creation. FD ownership, scheduler unification and the complete
backend execution/fault interface also remain open. Preserve current regression
gates while progressing toward Valkey V0–V11 and PostgreSQL P0–P12; neither workload
matrix nor architecture slice A is accepted by this result.
