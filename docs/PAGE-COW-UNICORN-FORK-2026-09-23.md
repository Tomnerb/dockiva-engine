# macOS page COW and bounded Unicorn fork — 2026-09-23

The native runtime now supports private guest processes on Unicorn, including
fork from a multithreaded parent. Prepared Redis 7.4.2 passes background RDB
snapshot and AOF rewrite tests, with acknowledged parent writes while the child
is active and data validation after restart. This does not qualify stock images,
Valkey, PostgreSQL, M46, M54 or production Docker replacement.

## Memory behavior

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

## Process and descriptor behavior

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

## Evidence and reproduction

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
