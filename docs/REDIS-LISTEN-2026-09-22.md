# Redis listener and stack allocation — 2026-09-22

Follow-up: [bounded guest threads](REDIS-THREADS-2026-09-22.md) resolves the clone
blocker below; Redis now reaches server initialization and stops at `fadvise64`.

The pinned Redis 7.4.2 replay now creates its loopback listener, registers it with
epoll and allocates a guarded background-thread stack. It then fails at the first
thread creation: Linux ARM64 syscall 220 (`clone`) with flags 4001536 (`0x3d0f00`)
returns EOPNOTSUPP from the single-process Unicorn adapter. Redis exits with
“Can't initialize Background Jobs”. No readiness or client query was reached;
service acceptance remains false.

## Changes

`native/net.h` now caps listen backlog requests at the existing guest policy of
128 rather than rejecting larger values. Linux's syscall takes a 32-bit int and
uses an unsigned comparison when capping: negative values also select the cap,
and upper register bits are discarded. Zero remains zero. Host queue capacity
may be lower or otherwise host-dependent; this does not promise an exact number
of queued connections. The runtime still requires an explicit loopback bind and
propagates host listen errors.

`native/mapping.h` accepts Linux MAP_STACK as a no-op on the dynamic mapping path.
It preserves allocation bounds and permissions. Thread-library guard pages still
use PROT_NONE and mprotect. Accepting the flag does not create guest threads.

`native/unicorn_backend.h` now traces intercepted clone/exec rejections when
`--trace` is enabled. Previously those rejections bypassed the shared syscall
trace, hiding the exact operation behind the application's error message.

## Validation

- The same freestanding ARM64 listener ELF runs on both runtime backends and a
  pinned Linux reference. Before the fix, both backends failed check 46 (backlog
  129), while Linux passed. Afterward all three pass **140 checks**: ten backlog
  inputs, real connect/accept/data, repeated listen and closed-descriptor errors.
- Native ASan/UBSan tests verify the actual backlog forwarded to host listen,
  including signed and upper-register-bit cases, plus unbound/wildcard rejection.
- A separate ARM64 MAP_STACK probe passes **12 checks** on both backends and Linux:
  zero-fill, access, guard protection, mprotect, retained data, unmap and invalid
  zero-length allocation.
- All **14** existing Unicorn adapter checks and **53** Python tests pass.
- Final binary: ad-hoc Hardened Runtime signature with the existing allow-jit
  entitlement, verified locally. Default execution remains interpreted.

The Redis replay uses the prepared pinned Linux ELF and no host-service or Docker
fallback. Docker is used only for the independent Linux behavior references.
This batch did not build Dockiva or qualify PostgreSQL/Valkey.

## Evidence and next work

`evidence/redis-listen/before.json` and `after.json` hold the listener comparison.
`acceptance.json` captures the first replay after listener correction;
`acceptance-stack.json` captures the stack change; `acceptance-final.json` includes
explicit clone diagnostics. `batch.json` records final source and binary hashes.
The MAP_STACK report is copied into this folder as `map-stack.json`.

Reproduce with `python3 scripts/verify_listen.py --linux`,
`python3 scripts/verify_map_stack.py --linux`, and
`python3 scripts/verify_redis_unicorn.py --trace --report evidence/redis-listen/acceptance-final.json`.

Next is bounded guest-thread execution in the Unicorn adapter: per-thread CPU,
FP/SIMD and TLS state, shared mappings/descriptors, clone parent/child return
semantics, thread IDs, exit/clear-child-TID and futex wait/wake. Blocking waits
must let another guest thread run. Independent thread probes should pass before
replaying Redis background jobs. Neither successful stack allocation nor returning
a fabricated clone success is sufficient.

References: [Linux listen implementation](https://raw.githubusercontent.com/torvalds/linux/v6.12/net/socket.c),
[listen backlog semantics](https://man7.org/linux/man-pages/man2/listen.2.html),
and [Linux MAP_STACK semantics](https://man7.org/linux/man-pages/man2/mmap.2.html).
