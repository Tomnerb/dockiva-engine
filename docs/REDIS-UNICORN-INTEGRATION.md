# Experimental CPU integration and Redis acceptance

2026-09-22 latest: [prepared Redis query and clean-restart persistence acceptance
now passes](REDIS-QUERY-PERSISTENCE-2026-09-22.md) with bounded guest threads.
The results below describe the earlier integration batch. Fork/COW, stock-image,
production and Dockiva packaging gates remain open.

2026-09-21. **CPU integration is implemented for a single guest process. Redis
service acceptance has not passed.** This is the next bounded implementation
batch after the profiling experiment, not completion of the service roadmap.

## Implemented

- A separate `vmfree-c-unicorn` build and explicit `--cpu-backend unicorn` option.
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

## Observed Redis result

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

## Acceptance and validation

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

## Reproduce

First build the [isolated patched dependency](../experiments/unicorn-profile/README.md).
Then, from the repository root:

```sh
python3 scripts/build_native.py --with-unicorn
codesign --force --sign - --options runtime --entitlements experiments/unicorn-adapter/jit.entitlements.plist build/vmfree-c-unicorn
python3 scripts/verify_unicorn_backend.py
python3 scripts/verify_redis_unicorn.py
```

The last command currently **fails**, preserving its result in
[`acceptance.json`](../evidence/redis-unicorn/acceptance.json). JIT and loopback
tests may require authorized execution outside the tool sandbox. The build
script also accepts explicit Unicorn include/library paths; it does not fetch
or install a dependency into the normal runtime.

## Next implementation boundary

The next work is coherent address-space/process/thread ownership for this CPU
backend, including the memory-advice behavior required by Redis's safety test,
fork state, thread CPU/TLS state, shared descriptors, blocking waits and futex
wakeup/exit semantics. Then add the required event-loop behavior and rerun this
same service acceptance. Returning success for unsupported synchronization or
disabling the safety check would not satisfy the gate.

The backend integration is useful progress, but this batch does not deliver the
requested working Redis service. Dockiva and its selected engine were not changed.
