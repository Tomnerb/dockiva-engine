# Private interpreter fork COW — 2026-09-22

Historical checkpoint: [PAGE-COW-UNICORN-FORK-2026-09-23.md](PAGE-COW-UNICORN-FORK-2026-09-23.md) supersedes the whole-backing/macOS and Unicorn-fork limitations below.

Private fork in the interpreter now retains guest memory backing and copies it
when writable access is requested. This implements the next step after
[reference-counted backing](MEMORY-BACKING-OWNERSHIP-2026-09-22.md). Copying is at
backing-allocation granularity: a segment allocation, the heap arena, or the mmap
arena. It is not page-granular COW, and general Unicorn fork remains unsupported.

## Implementation

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

## Validation and observed allocation behavior

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

## Next work and limits

Move toward page-granular storage and backend write-fault handling, then general
process/shared-object ownership in the database backend. A first heap write still
copies its 8 MiB arena; an mmap write can copy the configured 16–128 MiB arena.
This step therefore does not establish low-memory database performance.

General MAP_SHARED, shared futex identity, unified FD/scheduler ownership and the
full backend interface remain open. Architecture slice A and M54 are not accepted.
[Valkey V0–V11 and PostgreSQL P0–P12](WORKLOAD-GATES-V1.md) remain open; prepared
Redis continues to serve as regression evidence only.
