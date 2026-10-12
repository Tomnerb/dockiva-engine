# Redis concurrency and process-crash recovery — 2026-09-22

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

## Failure found and fixed

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

## Test harness and evidence

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

## Qualification boundary

These are short, bounded process-crash tests on a prepared fixture. They do not
simulate host power loss, torn storage writes or disk failure, and do not qualify
stock OCI images, background AOF rewrite/fork, sustained load, production isolation
or every Redis feature. No RAM, CPU, battery or throughput comparison was made.
The runtime is locally ad-hoc signed; the interpreter remains the default.
PostgreSQL/Valkey acceptance and Dockiva packaging remain separate open gates.

Next: package this Redis-tested adapter in Dockiva and repeat a focused end-to-end
UI start/connect/stop/restart test, preserving the experimental scope.

Reference: [Linux getpeername semantics](https://man7.org/linux/man-pages/man2/getpeername.2.html).
