# Redis epoll implementation — 2026-09-22

Follow-up: [listener and stack allocation](REDIS-LISTEN-2026-09-22.md) resolves
the backlog failure below and identifies guest thread creation as the next blocker.

Redis 7.4.2 now passes event-loop creation on the opt-in Unicorn backend. The
next observed failure is TCP listener setup: Redis requests backlog 511 and the
existing runtime rejects values above 128 with EINVAL. This replay did not reach
readiness or execute any client query. Redis service acceptance remains false;
PostgreSQL, Valkey and Dockiva qualification are unchanged.

## Implemented scope

`native/epoll.h` implements Linux ARM64 `epoll_create1` (20), `epoll_ctl` (21)
and `epoll_pwait` (22) at the shared syscall boundary. The ARM64 event structure
is 16 bytes, with the 64-bit user data at offset 8. Registrations support ADD,
MOD and DEL, level-triggered IN/PRI/OUT, and unconditional ERR/HUP delivery.
Host sockets use poll; guest buffered pipes use their actual readiness state.
Zero, finite and indefinite timeouts are supported. Waiting does not consume
instruction steps; external process termination remains the cancellation path.

There are eight epoll instances, each with at most 64 registrations, sharing the
existing bounded descriptor table. Epoll descriptor aliases share instance state.
Pipe registrations survive closing one descriptor while another alias remains;
closing the last alias removes them. Socket close removes registrations before
host or guest descriptor reuse. Close-on-exec releases epoll descriptors. A
rotating cursor prevents starvation when maxevents truncates a ready set.

Unsupported flags (including edge-triggered, one-shot and RDHUP), nested epoll,
non-null signal masks, scheduler mode and cloning with live epoll instances fail
explicitly with EOPNOTSUPP. Regular files are not epoll targets. Existing loopback
TCP restrictions remain. This is a bounded single-process interface, not full
Linux epoll or thread support. Its fixed tables are static storage, not dynamically
charged to the guest allocation budget.

## Validation

- One freestanding ARM64 ELF passes **40 checks** on the interpreter, Unicorn
  and a pinned Linux reference: ABI, errors, timeout, repeated readiness, data,
  MOD/DEL, aliases, HUP/ERR, descriptor reuse, fcntl and fairness.
- Native socket tests pass with AddressSanitizer and UndefinedBehaviorSanitizer:
  read/write readiness, finite timeout, immediate readiness with infinite timeout,
  HUP, descriptor reuse, unsupported modes, CLOEXEC and repeated instance cleanup.
  These use host socket pairs; real Redis separately exercises loopback TCP setup.
- Existing backend, process/scheduler and Python regressions are recorded in the
  batch evidence. The normal backend remains the interpreter.
- The final Unicorn executable is ad-hoc signed with Hardened Runtime and only
  the existing allow-jit entitlement. This does not establish distribution readiness.

Evidence: `evidence/redis-epoll/abi.json`, `host-test.txt`, `acceptance.json`, and
`batch.json`. Reproduce with `python3 scripts/verify_epoll.py --linux` and
`python3 scripts/verify_redis_unicorn.py --trace --report evidence/redis-epoll/acceptance.json`.
Docker is used only as the Linux behavior reference, never as the Redis replay's
execution fallback. No Dockiva package was built in this batch.

The next bounded change is listen-backlog handling: verify Linux's capping behavior,
map it to an explicit bounded guest policy, then replay Redis to expose any further
startup dependency. Passing epoll tests alone is not database acceptance.

ABI and behavior references: [Linux eventpoll UAPI](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/eventpoll.h),
[epoll_ctl](https://man7.org/linux/man-pages/man2/epoll_ctl.2.html),
and [epoll_wait](https://man7.org/linux/man-pages/man2/epoll_wait.2.html).
