# Redis VM-free query and restart acceptance — 2026-09-22

Follow-up: [four/eight-client concurrency and SIGKILL recovery](REDIS-CONCURRENCY-RECOVERY-2026-09-22.md)
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

## Changes

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

## Validation and evidence

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

## Scope and next gate

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
