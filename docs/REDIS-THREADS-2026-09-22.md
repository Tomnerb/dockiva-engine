# Bounded Unicorn guest threads — 2026-09-22

Follow-up: [query and restart acceptance](REDIS-QUERY-PERSISTENCE-2026-09-22.md)
resolves the file-advice and peer-address blockers; the prepared Redis test now passes.

Redis 7.4.2 now creates five background threads (guest TIDs 2–6) and reports
“Server initialized”. The acceptance replay then stops at unimplemented ARM64
syscall 223 (`fadvise64`) during persistence-file setup. Readiness, client queries
and restart persistence have **not** passed. PostgreSQL/Valkey qualification and
the Dockiva package are unchanged.

## Implemented

The opt-in Unicorn adapter supports one thread group with eight live threads.
Each thread has a full Unicorn CPU context (including FP/SIMD and TLS), signal
mask, wait state and clear-child-TID address. Memory and descriptors are shared.
Switching happens at syscalls, host execution timeouts and a 10,000-instruction
quantum. The existing instruction budget is shared across all threads. TIDs grow
monotonically, capped at 65535; freed live slots and their CPU contexts can be reused.
`sysinfo` includes the live guest threads.

The supported clone profile requires VM, FS, FILES, SIGHAND and THREAD sharing;
optional SYSVSEM, SETTLS, PARENT_SETTID, CHILD_SETTID and CHILD_CLEARTID are accepted.
Parent/child return values, TID stores, gettid, set_tid_address, per-thread exit,
exit_group and sched_yield are implemented. Thread exit clears its registered TID
and wakes a matching non-private futex waiter. Fork and exec remain unsupported;
clone3 returns ENOSYS so libraries may try clone.

Futex WAIT, WAKE, WAIT_BITSET and WAKE_BITSET support private/shared keys within
this single process, masks, wake counts, relative WAIT timeouts and absolute
monotonic/realtime WAIT_BITSET timeouts. Compare-and-enqueue is serialized in the
host scheduler. Priority-inheritance/requeue and other operations return ENOSYS.
Per-thread socket waits and nonblocking epoll retries let other guests progress.

This is still experimental thread support, not full Linux pthread compatibility.
There are no guest signal handlers/cancellation, robust mutex recovery, process
sharing or production thread qualification. Host file I/O may block the host
scheduler. Descriptor-close/reuse races during outstanding I/O and broader atomic
memory-model behavior need additional qualification. Idle waits currently use a
1 ms poll interval; no CPU, RAM, battery or performance improvement is claimed.
Unicorn-owned contexts are bounded in count but outside the guest allocation
budget, like other Unicorn allocations.

## Validation

- The same ARM64 thread probe passes on Unicorn and Linux: clone returns, shared
  memory, thread IDs, parent/child TID stores, TLS and FP preservation, futex
  handoff, epoll waiting while another thread makes a pipe ready, and clear-TID join.
- Eleven futex errno/timeout checks pass on Unicorn and Linux, including both
  clock choices for expired absolute waits, relative waits, alignment and EFAULT.
- A runtime-only limit probe creates seven children, exercises instruction
  preemption of spinning children, and verifies EAGAIN at the eight-thread limit.
- All three probes also pass with the runtime compiled under ASan/UBSan. The
  statically linked Unicorn library itself is not sanitizer-instrumented.
- The existing 40-check epoll probe passes on both runtime backends; 14 adapter
  checks and 53 Python tests pass. The old futex WAIT-unsupported regression was
  replaced with an unsupported PI-operation check; real WAIT is tested above.
- The final normal Unicorn binary is locally ad-hoc signed with Hardened Runtime
  and the existing allow-jit entitlement. The interpreter remains the default.

Evidence lives in `evidence/redis-threads/`: `probes.json`,
`sanitizer-probes.json`, `epoll-tests.json`, `adapter-tests.json`,
`python-tests.txt`, `acceptance.json` and `batch.json`.
Reproduce with `python3 scripts/verify_unicorn_threads.py --linux` and
`python3 scripts/verify_redis_unicorn.py --trace --report evidence/redis-threads/acceptance.json`.
Docker is only the independent Linux probe reference, never a Redis fallback.

Next: implement and verify the file-advice syscall used by Redis, then rerun
unchanged PING/SET/GET, shutdown and restart-persistence acceptance. “Server
initialized” is progress, not a successful Redis service result.

References: [clone ABI](https://man7.org/linux/man-pages/man2/clone.2.html),
[FUTEX_WAIT](https://man7.org/linux/man-pages/man2/FUTEX_WAIT.2const.html),
[FUTEX_WAIT_BITSET](https://man7.org/linux/man-pages/man2/FUTEX_WAIT_BITSET.2const.html),
[clear-child-TID](https://man7.org/linux/man-pages/man2/set_tid_address.2.html).
