# Redis bring-up

How Redis was brought up on the runtime, in the order the work happened.

- [Experimental CPU integration and Redis acceptance](#redis-unicorn-integration)
- [Redis startup-check investigation — 2026-09-22](#redis-startup-check)
- [Redis listener and stack allocation — 2026-09-22](#redis-listen)
- [Redis epoll implementation — 2026-09-22](#redis-epoll)
- [Guest sysinfo and Redis progress — 2026-09-22](#redis-sysinfo)
- [Bounded Unicorn guest threads — 2026-09-22](#redis-threads)
- [Redis VM-free query and restart acceptance — 2026-09-22](#redis-query-persistence)
- [Redis concurrency and process-crash recovery — 2026-09-22](#redis-concurrency-recovery)
- [Local Dockiva Redis test — 2026-09-22](#dockiva-redis-preview)

<a id="redis-unicorn-integration"></a>

## Experimental CPU integration and Redis acceptance

2026-09-22 latest: [prepared Redis query and clean-restart persistence acceptance
now passes](#redis-query-persistence) with bounded guest threads.
The results below describe the earlier integration batch. Fork/COW, stock-image,
production and Dockiva packaging gates remain open.

2026-09-21. **CPU integration is implemented for a single guest process. Redis
service acceptance has not passed.** This is the next bounded implementation
batch after the profiling experiment, not completion of the service roadmap.

### Implemented

- A separate `dockiva-runtime-unicorn` build and explicit `--cpu-backend unicorn` option.
  The standard build/default backend remains the interpreter. The standard build
  rejects an unavailable Unicorn backend explicitly.
- Both execution engines use the same Linux syscall dispatcher, extracted into
  `native/syscall.h`. The adapter reuses the existing ELF loader, bounded memory
  arenas, guest-pointer validation and filesystem/socket adapters.
- Guest mappings use the runtime-owned host buffers. Mapping, unmapping and
  permission changes synchronize to Unicorn at the syscall boundary. Guest
  instructions therefore observe syscall writes without a second memory copy.
- Integer registers, stack, flags and TLS synchronize across the syscall boundary;
  SIMD/FP state remains in the same engine. A test verifies FDIV and TLS across a
  syscall. Only Linux SVC #0 is accepted; unexpected CPU exceptions fail explicitly.
- An instruction hook enforces the shared total guest instruction budget across
  syscall calls. One-second engine timeouts yield back to the host. The earlier
  uninstrumented microbenchmark performance is **not** a measurement of this
  adapter: instruction-budget hooks and mapping rebuilds add overhead.
- With no guest threads, a valid FUTEX_WAKE/FUTEX_WAKE_PRIVATE returns zero
  waiters. Futex waits and other operations explicitly return ENOSYS. This does
  not implement M36 or pthread synchronization.

The adapter explicitly rejects clone/exec with EOPNOTSUPP. Existing interpreter
process scheduling, terminal/job-control and interpreter profiling options cannot
be selected with this backend. It requires `--dynamic` for page-aligned loader
mappings, including when running a static ELF. There is no silent interpreter,
Docker, or host-native database fallback.

### Observed Redis result

The pinned, unchanged ARM64 Redis **7.4.2** executable now prints its version and
exits zero. It proceeds beyond the former unsupported floating-point instruction.

The AOF service test exits 1 before readiness because Redis cannot complete its
ARM64 copy-on-write safety test. The diagnostic trace shows guest `madvise`
(syscall 233, advice 8 / MADV_FREE) receiving the existing unsupported result
before Redis reports the failure. This does not establish the behavior of later
startup paths. Fork/clone and general guest threads also remain unsupported in
this backend. No safety-warning suppression was added.

**Zero service queries passed; persistence was not reached.** Version success
must not be described as Redis service support, PostgreSQL support, or Dockiva
readiness. The tested root is a prepared ELF fixture, not the full stock image.

### Acceptance and validation

`scripts/verify_redis_unicorn.py` verifies fixture hashes, copies a disposable
root, starts loopback-only Redis with AOF and `appendfsync always`, and requires:

1. A readiness log from the launched process and real RESP PING/SET/GET replies.
2. Successful Redis SHUTDOWN and process exit.
3. A fresh runtime process using the same data directory, with PING/GET confirming
   the value survived restart, then successful shutdown.

The current result is a deliberate nonzero acceptance failure. No test stage is
marked successful from version output or logs alone. The harness bounds each
startup, client operation and shutdown and cleans up its own processes/data.
It does not yet qualify concurrent clients, expiry, RDB, abrupt-crash recovery,
stock entrypoints or long-running resource usage.

Validation completed:

- All 53 existing Python tests passed with loopback access. An initial sandboxed
  run had five permission failures because socket binding was prohibited.
- All 14 real-ELF adapter checks passed with the ad-hoc hardened executable,
  including status propagation, exact loop-budget rejection, invalid instructions,
  SVC rejection, unmapped access, mmap visibility, mprotect/munmap revocation,
  non-executable data, clone rejection, FP/TLS preservation and futex scope.
- Existing native process, startup, filesystem/sync/permissions and cancellation
  checks passed with address/undefined-behavior sanitizers. These validate the
  retained runtime code; the linked Unicorn library itself was not ASan-built.
- The integrated executable passed strict ad-hoc signature verification with
  hardened runtime and only the allow-jit entitlement. No Developer ID,
  notarization, App Sandbox or Dockiva distribution test was performed.

Unicorn's internal allocations are outside the runtime allocator accounting.
Neither a total-RSS limit nor confinement of untrusted images is established.
The dependency is the locally patched Unicorn 2.1.4 source build from the previous
experiment; build metadata records its library hash. Production licensing and
upstream patch qualification remain open.

### Reproduce

First build the [isolated patched dependency](../experiments/unicorn-profile/README.md).
Then, from the repository root:

```sh
python3 scripts/build_native.py --with-unicorn
codesign --force --sign - --options runtime --entitlements experiments/unicorn-adapter/jit.entitlements.plist build/dockiva-runtime-unicorn
python3 scripts/verify_unicorn_backend.py
python3 scripts/verify_redis_unicorn.py
```

The last command currently **fails**, preserving its result in
[`acceptance.json`](../evidence/redis-unicorn/acceptance.json). JIT and loopback
tests may require authorized execution outside the tool sandbox. The build
script also accepts explicit Unicorn include/library paths; it does not fetch
or install a dependency into the normal runtime.

### Next implementation boundary

The next work is coherent address-space/process/thread ownership for this CPU
backend, including the memory-advice behavior required by Redis's safety test,
fork state, thread CPU/TLS state, shared descriptors, blocking waits and futex
wakeup/exit semantics. Then add the required event-loop behavior and rerun this
same service acceptance. Returning success for unsupported synchronization or
disabling the safety check would not satisfy the gate.

The backend integration is useful progress, but this batch does not deliver the
requested working Redis service. Dockiva and its selected engine were not changed.

<a id="redis-startup-check"></a>

## Redis startup-check investigation — 2026-09-22

Follow-up: [guest sysinfo is now implemented](#redis-sysinfo).
The latest Redis stop is epoll_create1 (20); the syscall-179 stop below is the
earlier result. No service or fork/COW qualification follows from either batch.

**The original startup stop is resolved through Redis's own unsupported-feature
fallback. Redis is still not a working service. Fork/copy-on-write has not been
implemented or validated by this result.**

### What the source and trace showed

The pinned Redis 7.4.2 check attempts `madvise(MADV_FREE)`. If the advice is
unsupported and returns EINVAL, Redis accepts that capability boundary and
continues. If MADV_FREE succeeds, Redis instead forks and inspects shared dirty
pages through `/proc` to test an old ARM64 kernel bug. These are different paths.
Our previous claim that passing this startup boundary necessarily required
implementing fork was too broad.

Previously the entire madvise syscall returned ENOSYS, causing Redis to report
that it could not complete the check. The new limited interface supports mapped
range access-pattern hints and reports EINVAL for unsupported advice. It does
**not** return success for MADV_FREE, fabricate shared-dirty accounting, change
Redis, or set `ignore-warnings ARM64-COW-BUG`.

Sources: [pinned Redis check](https://github.com/redis/redis/blob/7.4.2/src/syscheck.c),
[Linux madvise contract](https://man7.org/linux/man-pages/man2/madvise.2.html).
Downloaded Redis source hashes are in
[upstream.json](../evidence/redis-cow/upstream.json).

### Implementation

Both runtime engines now use `guest_madvise` in `native/mapping.h` through the
shared Linux syscall boundary:

- MADV_NORMAL, MADV_RANDOM and MADV_SEQUENTIAL accept mapped guest ranges. They
  are access-pattern hints; this eagerly backed runtime changes no read-ahead
  policy or application-visible bytes.
- Alignment, length overflow, unmapped ranges and holes are checked. PROT_NONE
  remains mapped. Advice uses Linux's signed 32-bit argument interpretation.
- Unsupported advice—including MADV_FREE and MADV_DONTNEED—returns EINVAL.
  No guest address or advice number is forwarded to the host's madvise ABI.
- Optional syscall tracing records the advice value and returned error.

This is limited memory-advice support, not reclamation support. Full fork/COW,
guest threads, futex waiting, and service event handling remain open.

### Observed result

The unchanged, hash-verified Redis executable takes the EINVAL fallback. It emits
its startup/version/configuration messages, then exits 125 at **sysinfo, Linux
syscall 179**, which remains unimplemented.

The final trace includes `madvise advice=8 result=-22` and the later `syscall=179`.
The earlier safety-check failure message is absent. We do not count this as
execution of the fork/shared-dirty test branch.

The real RESP/AOF acceptance result remains **false**: zero successful service
queries and no restart persistence result. Dockiva was not modified or rebuilt.
No Docker or host-native database fallback served application requests. Docker
was used only for the independent Linux ABI reference fixture.

### Validation

- Thirteen cases ran through the interpreter, Unicorn and the pinned Linux
  reference. Twelve cases match Linux, covering hints, content preservation,
  unknown advice, alignment, zero length, overflow, holes, PROT_NONE and integer
  argument width. The final capability difference is explicit: Linux supports
  MADV_FREE; the runtime returns EINVAL. This is not labelled Linux-equivalent.
- A native address/undefined-behavior sanitizer fixture passed mapped-segment,
  brk, mmap, bounds, unchanged-byte and allocation-cleanup checks.
- All 14 real-ELF CPU adapter checks passed on the hardened ad-hoc signed build.
- All 53 existing Python tests passed with required loopback access.
- The reference container was removed by the verifier; runtime roots and test
  server processes were disposable and cleaned up.

Reproduce the focused comparisons with `python3 scripts/verify_madvise.py --linux`.
The identical ELF runs in both engines and Linux. This uses the existing local
cross-compiler/linker and the previously pinned ARM64 reference image.
For the native sanitizer fixture:

```sh
clang -std=c11 -O2 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined tests/native/madvise.c -o build/redis-cow/madvise-sanitize
build/redis-cow/madvise-sanitize
```

Evidence: [ABI comparison](../evidence/redis-cow/madvise.json),
[Redis acceptance and trace](../evidence/redis-cow/acceptance-trace.json),
[batch metadata](../evidence/redis-cow/batch.json).

### Next boundary

Implement guest-scoped sysinfo reporting, then rerun the same pinned Redis
acceptance to identify its next actual dependency. Keep the full service goal
as PING/SET/GET plus AOF persistence across restart. Do not promote database or
threading milestones based on this fallback result.

<a id="redis-listen"></a>

## Redis listener and stack allocation — 2026-09-22

Follow-up: [bounded guest threads](#redis-threads) resolves the clone
blocker below; Redis now reaches server initialization and stops at `fadvise64`.

The pinned Redis 7.4.2 replay now creates its loopback listener, registers it with
epoll and allocates a guarded background-thread stack. It then fails at the first
thread creation: Linux ARM64 syscall 220 (`clone`) with flags 4001536 (`0x3d0f00`)
returns EOPNOTSUPP from the single-process Unicorn adapter. Redis exits with
“Can't initialize Background Jobs”. No readiness or client query was reached;
service acceptance remains false.

### Changes

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

### Validation

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

### Evidence and next work

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

<a id="redis-epoll"></a>

## Redis epoll implementation — 2026-09-22

Follow-up: [listener and stack allocation](#redis-listen) resolves
the backlog failure below and identifies guest thread creation as the next blocker.

Redis 7.4.2 now passes event-loop creation on the opt-in Unicorn backend. The
next observed failure is TCP listener setup: Redis requests backlog 511 and the
existing runtime rejects values above 128 with EINVAL. This replay did not reach
readiness or execute any client query. Redis service acceptance remains false;
PostgreSQL, Valkey and Dockiva qualification are unchanged.

### Implemented scope

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

### Validation

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

<a id="redis-sysinfo"></a>

## Guest sysinfo and Redis progress — 2026-09-22

**Linux ARM64 sysinfo is implemented with an explicit guest resource policy.
Redis proceeds past syscall 179 and now stops at epoll_create1, syscall 20.
The service is not ready: no PING/SET/GET or persistence result passed.**

### Implemented behavior

`native/sysinfo.h` supplies the shared syscall dispatcher used by both CPU engines.
It explicitly writes the 112-byte Linux AArch64 LP64 layout; it does not copy a
Darwin structure. Writable unaligned buffers are accepted. Null, read-only,
unmapped and insufficiently writable ranges return Linux EFAULT. Padding is
initialized; bytes beyond the structure are left untouched.

The virtual environment reports:

| Field | Guest policy |
| --- | --- |
| uptime | Whole monotonic seconds since this runtime invocation started; preserved across guest process activity |
| totalram | Configured accounted allocation budget (`--memory-limit-mib`, default 256 MiB) |
| freeram | Unused portion of that budget, saturated at zero |
| mem_unit | 1 byte |
| procs | Root plus retained guest process slots, including waitable children; includes the legacy synchronous child while executing |
| totalswap, freeswap | Swap allowed beyond the budget (`--memory-swap-mib`, default 0; dockivad passes Docker's `--memory-swap` minus `-m`) and its unused part: charges past totalram count as swap in use |
| high memory, shared-memory pool, buffer cache | Zero: these guest facilities are not supplied by this runtime |
| load averages | Zero placeholder; load-average accounting is not implemented |

Memory accounting includes the runtime's retained arenas and allocation headers.
It excludes Unicorn's internal allocations and is **not host RAM, host RSS, or
guaranteed allocatable space for any single guest allocation**. Existing ELF,
heap and mmap limits still apply. Returning these guest-policy values is not a
claim of complete Linux system-statistics equivalence or whole-product memory
enforcement. A guest that requires accurate load averages is not qualified.

ABI references: [Linux UAPI structure](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/sysinfo.h)
and [sysinfo manual](https://man7.org/linux/man-pages/man2/sysinfo.2.html).

### Database result

The unchanged, hash-verified Redis 7.4.2 fixture still uses its documented
unsupported-MADV_FREE fallback. It now completes sysinfo, loads its configuration,
and initializes its monotonic clock. It then calls epoll_create1 and exits 125
because that syscall is not implemented. Redis also reports its fallback for an
unsupported NOFILE limit query; that warning is recorded, not suppressed.

The acceptance harness still requires real PING/SET/GET replies, successful
shutdown, a new runtime process and an AOF-backed GET after restart. The final
result remains `service_qualified: false`, with zero successful queries.
There is no Docker or host-native database fallback. Dockiva was not rebuilt.

### Validation

- One independently compiled freestanding ELF exercised the same sysinfo calls on
  the interpreter, Unicorn and the pinned ARM64 Linux reference. The guest C
  structure has compile-time size/offset assertions. All three passed return-code,
  field-invariant, unaligned-buffer/canary and four EFAULT checks.
- Runtime-specific assertions verified a configured 64 MiB total, lower available
  budget after arena allocation, one initial guest process and byte units.
  Linux's host values were intentionally not compared numerically to this policy.
- Native ASan/UBSan checks covered layout, zeroed padding, uptime, allocation and
  release accounting, saturation, guest process slots and writable-buffer checks.
- All 14 CPU adapter tests and all 53 existing Python tests passed.
- The integrated Unicorn executable passed strict ad-hoc hardened signature
  verification with the JIT entitlement. This is not distribution qualification.

The Linux reference container was disposable and removed. Database test roots and
launched runtime processes were cleaned up by the acceptance harness.

Reproduce after building the normal and opt-in executables:

```sh
python3 scripts/verify_sysinfo.py --linux
clang -std=c11 -O2 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined tests/native/sysinfo.c -o build/redis-sysinfo/native-sanitize
build/redis-sysinfo/native-sanitize
python3 scripts/verify_redis_unicorn.py --trace
```

The Redis command currently exits nonzero. JIT, Docker-reference and loopback
checks require the corresponding execution permissions.

Evidence: [ABI results](../evidence/redis-sysinfo/abi.json),
[Redis acceptance and trace](../evidence/redis-sysinfo/acceptance.json),
[CPU checks](../evidence/redis-sysinfo/adapter-tests.json),
[batch metadata](../evidence/redis-sysinfo/batch.json).

### Next implementation target

Implement epoll as an interface: creation/close, descriptor registration and
modification, readiness waiting, timeouts and invalid-descriptor behavior. Check
those semantics against Linux and rerun this same Redis acceptance. Creating an
epoll descriptor alone would not provide event-loop support. Threading and futex
waiting remain later known requirements; PostgreSQL remains unqualified.

<a id="redis-threads"></a>

## Bounded Unicorn guest threads — 2026-09-22

Follow-up: [query and restart acceptance](#redis-query-persistence)
resolves the file-advice and peer-address blockers; the prepared Redis test now passes.

Redis 7.4.2 now creates five background threads (guest TIDs 2–6) and reports
“Server initialized”. The acceptance replay then stops at unimplemented ARM64
syscall 223 (`fadvise64`) during persistence-file setup. Readiness, client queries
and restart persistence have **not** passed. PostgreSQL/Valkey qualification and
the Dockiva package are unchanged.

### Implemented

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

### Validation

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

<a id="redis-query-persistence"></a>

## Redis VM-free query and restart acceptance — 2026-09-22

Follow-up: [four/eight-client concurrency and SIGKILL recovery](#redis-concurrency-recovery)
now pass for the prepared fixture; Dockiva packaging remains open.

The prepared, pinned Linux ARM64 Redis 7.4.2 fixture passes the existing service
acceptance test on the opt-in Unicorn adapter. Both a traced run and a normal
run completed:

1. Redis startup and readiness on a loopback TCP listener.
2. PING → PONG, SET → OK, GET → the exact written value.
3. SHUTDOWN, client EOF and runtime exit status zero.
4. A new runtime process using the same temporary data directory.
5. PING and GET of the unique key written before shutdown.
6. A second clean shutdown and zero exit status.

The test uses AOF with `appendfsync always`, disables periodic RDB saves, and
checks real RESP replies. The logs confirm loading the AOF base and incremental
files on restart. No native Redis or Docker execution fallback was used. This is
clean-restart persistence, not power-loss/crash recovery or a durability guarantee
under all failure conditions.

### Changes

`native/fadvise.h` implements ARM64 syscall 223 for filesystem descriptors and
regular-file aliases. It validates the six Linux advice values, signed length,
32-bit fd/advice arguments, descriptor lifetime and pipe errors. Valid advice is
accepted as a nonbinding hint; host cache/readahead behavior is unchanged.
DONTNEED does not discard data, force writeback, or replace fsync. Directories
are supported; socket and other special descriptor advice remains explicitly
unsupported. This carries no memory-saving or performance claim.

Redis subsequently reached readiness but could not accept a client because the
runtime previously supported only NULL peer outputs. `native/net.h` now packs
accepted IPv4 peer addresses into the Linux 16-byte sockaddr layout, including
port/address byte order, truncated and zero-length buffers, and the actual output
length. A NULL address ignores the length pointer. Failed output copies close
the accepted host socket. Existing accept4 flags, nonblocking behavior and
loopback restrictions remain in force.

### Validation and evidence

- **43** file-advice checks pass on the interpreter, Unicorn and Linux: all six
  hints, beyond-EOF/range cases, invalid arguments, duplicate/closed descriptors,
  pipes, directories, and preservation of file data and file offset.
- **69** accept checks pass on both backends and Linux: exact peer address,
  truncation, guard bytes, zero/NULL buffers, bad pointers/lengths, flags and real
  accepted-connection data exchange.
- Both probes pass under ASan/UBSan on both runtime backends. The linked Unicorn
  library itself remains unsanitized.
- Three bounded-thread probes, 14 CPU-adapter checks and 53 Python tests pass.
- The normal Unicorn binary is locally ad-hoc signed with Hardened Runtime and
  the existing allow-jit entitlement, and its signature was verified.

`evidence/redis-fadvise/acceptance-peer.json` records the traced successful replay;
`acceptance-final.json` records the normal successful replay. The earlier
`acceptance.json` preserves the intermediate client-accept failure.
`abi.json`, `accept-tests.json`, `sanitizer-tests.json`, `thread-tests.json`,
`adapter-tests.json`, `python-tests.txt` and `batch.json` hold supporting evidence
and final source/binary hashes.

Reproduce with `python3 scripts/verify_fadvise.py --linux`,
`python3 scripts/verify_accept.py --linux`, and
`python3 scripts/verify_redis_unicorn.py --report evidence/redis-fadvise/acceptance-final.json`.
Docker is used only as the independent Linux ABI reference for the probes.

### Scope and next gate

This qualifies the prepared Redis fixture for this bounded query/clean-restart
scenario. It does not qualify a stock OCI image, every Redis feature, production
isolation, fork-based background save/rewrite, sustained concurrent load,
crash recovery, PostgreSQL or Valkey. The default runtime remains interpreted.
This adapter has not been packaged in Dockiva. Milestone-wide gates remain open.

Next: concurrent-client and repeated-restart tests, then interrupted-process/AOF
recovery tests, before updating the Dockiva test package. Measure performance,
RAM and idle CPU separately; this functional result supplies none of those claims.

References: [Linux file-advice semantics](https://man7.org/linux/man-pages/man2/posix_fadvise.2.html),
[Linux implementation](https://raw.githubusercontent.com/torvalds/linux/v6.12/mm/fadvise.c),
and [accept peer-output semantics](https://man7.org/linux/man-pages/man2/accept.2.html).

<a id="redis-concurrency-recovery"></a>

## Redis concurrency and process-crash recovery — 2026-09-22

The prepared Redis 7.4.2 Linux ARM64 fixture passes bounded concurrent-client,
repeated-clean-restart and forced-process-termination recovery tests on the
VM-free Unicorn adapter. Four-client and eight-client runs each use six runtime
processes over one temporary data directory: three clean write/shutdown cycles,
two write/SIGKILL cycles, and a final recovery/read/shutdown cycle.

| Final run | Concurrent clients | Regular per-client SET/GET pairs | Shared-counter INCR replies | Verified keys at final recovery | Deliberate SIGKILL exits |
| --- | ---: | ---: | ---: | ---: | ---: |
| Four clients | 4 | 300 | 300 | 351 | 2 |
| Eight clients | 8 | 600 | 600 | 651 | 2 |

Every normal workload starts its clients behind a barrier. Clients use distinct
SET/GET keys and also contend on one shared INCR key. Counter replies must be
unique and cover the exact expected range; their final values must persist.
The three counter keys are included in the final key totals.

Each crash workload terminates only its test-owned runtime with SIGKILL after at
least 24 SET replies have been observed. Both final runs observed exactly 24 per
crash, with interrupted clients still connected. The harness verifies **every**
observed acknowledged key after restart, including keys from all earlier cycles.
Unanswered writes are not required either to survive or disappear. Each clean
shutdown must return EOF and process status zero; each deliberate kill must
return status -9. Each run recovered 48 additional acknowledged keys across its
two crashes. AOF is enabled with `appendfsync always`; periodic RDB saves are off.

### Failure found and fixed

The initial eight-client run exited during its third clean cycle at unsupported
ARM64 syscall 205, `getpeername`. That failed run is retained as
`evidence/redis-recovery/eight-clients-before.json`.

`native/net.h` now translates the connected IPv4 peer address into Linux's
sockaddr layout, with actual length reporting, truncated/zero-length buffers,
negative-length rejection and host errors. The shared address-query path also
rejects negative lengths for getsockname. The shared syscall dispatcher routes
getpeername to this adapter.

The independent accept/peer-query ELF now passes **110 checks** on the interpreter,
Unicorn and Linux, including both connection directions, unconnected sockets,
exact address data, truncation, guard bytes and invalid pointers/lengths. Both
runtime backends pass the same probe under ASan/UBSan; the linked Unicorn library
itself is not sanitizer-instrumented.

### Test harness and evidence

`scripts/verify_redis_recovery.py` validates the pinned fixture before launching
it, records the final runtime hash, and bounds clients, writes, cycle counts,
readiness, workload duration, individual network calls and cleanup. Each process
has a shared one-billion-instruction limit. It records runtime logs, recovered-key
counts, an acknowledged-data digest and AOF file sizes/hashes. It never invokes
Docker or a host Redis service. Docker is only the independent ABI reference.

Five verifier unit tests ensure missing/corrupt values and early runtime exit fail,
and validate RESP integer parsing used for INCR. Mocked tests are not counted as
database acceptance. All **58** Python tests and **14** CPU-adapter checks pass.
The original PING/SET/GET/clean-restart test also passes on the final binary.

Reproduce with:

```
python3 scripts/verify_redis_recovery.py --report evidence/redis-recovery/four-clients.json
python3 scripts/verify_redis_recovery.py --clients 8 --report evidence/redis-recovery/eight-clients.json
python3 scripts/verify_accept.py --linux
```

The two final recovery reports, `peer-tests.json`, `sanitizer-tests.json`,
`python-tests.txt`, `adapter-tests.json`, `basic-acceptance.json` and `batch.json`
are in `evidence/redis-recovery/`. The earlier `acceptance.json` is the initial
four-client run without shared-counter checks, not the final-build result.

### Qualification boundary

These are short, bounded process-crash tests on a prepared fixture. They do not
simulate host power loss, torn storage writes or disk failure, and do not qualify
stock OCI images, background AOF rewrite/fork, sustained load, production isolation
or every Redis feature. No RAM, CPU, battery or throughput comparison was made.
The runtime is locally ad-hoc signed; the interpreter remains the default.
PostgreSQL/Valkey acceptance and Dockiva packaging remain separate open gates.

Next: package this Redis-tested adapter in Dockiva and repeat a focused end-to-end
UI start/connect/stop/restart test, preserving the experimental scope.

Reference: [Linux getpeername semantics](https://man7.org/linux/man-pages/man2/getpeername.2.html).

<a id="dockiva-redis-preview"></a>

## Local Dockiva Redis test — 2026-09-22

The prepared Redis 7.4.2 fixture is now packaged with the experimental Unicorn
adapter in a separate local Apple Silicon Dockiva test application.

Open `/Users/macbookpro16m2max/GolandProjects/devstack/devstack/bin/Launch Dockiva Redis Test.command`.
This launches `dockiva-redis-test.app` using `~/.vmfree-dockiva-redis-test` for
engine state. Existing app artifacts and engine stores remain separate.
Select VM-free, open Create container, choose **Redis preview**, and create it.
Leave Command and port mappings empty. The bundled engine starts automatically
when VM-free is selected; no Python, Homebrew or terminal setup is required.
Connect a Redis client to `127.0.0.1:16379` after Redis finishes loading.
Only one server can use that port at a time.

The digest-pinned `vmfree/redis:preview` image alone selects Unicorn, dynamic
loading and bounded larger ELF/mmap limits. Other image profiles retain the
interpreter. Redis uses its own writable root and `/data` with AOF persistence
and `appendfsync always`. Stop/Start and Restart keep data; Remove deletes it.
Dockiva Stop currently terminates the runtime; the persistence test verifies
recovery of acknowledged writes after this operation, not guest SHUTDOWN semantics.
Execution limits still apply, including 60 host CPU seconds and a billion guest
instructions per run. This is a bounded development preview, not a durable service
deployment. Background fork/AOF rewrite remains unsupported.

Validation against the final signed app's helper passed: automatic engine startup
with developer tools absent from PATH; shell and HTTP regressions; Redis
PING/SET/GET; listener closure after Stop; and saved-value recovery after Start
and Restart. Readiness checks wait through Redis's LOADING response.
Four Go VM-free tests passed; the separate live-engine test was skipped because
no external engine was supplied. 59 Python regression tests and four new profile
guard tests passed. The frontend and production Go application built successfully.
Ad-hoc deep signature verification passed, with the Unicorn JIT entitlement
restored after bundle signing. Go tests emitted existing SDK minimum-version
linker warnings; older macOS versions were not tested.

The application was launched and its process confirmed. Native GUI clicks were
not automated. Stock Redis images, PostgreSQL, Valkey, production isolation,
distribution/licensing, RAM/CPU/battery savings and actual 8 GB hardware remain
outside this result. No milestone or production acceptance is implied.

Evidence and artifact hashes: [report](../evidence/dockiva-redis/report.json),
[packaged adapter tests](../evidence/dockiva-redis/adapter-tests.txt),
[Python regressions](../evidence/dockiva-redis/python-tests.txt),
[profile guards](../evidence/dockiva-redis/profile-tests.txt).

To rebuild the helper, first prepare the pinned database fixture using the
existing database-fixture workflow, then run `scripts/bundle_dockiva_engine.py`
from an Apple Silicon Python environment with PyInstaller 6.22.3. The builder
verifies fixture hashes and creates the prepared OCI image. Dockiva's macOS
packaging task preserves the nested Unicorn JIT entitlement.
