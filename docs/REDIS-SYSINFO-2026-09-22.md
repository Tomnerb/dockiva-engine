# Guest sysinfo and Redis progress — 2026-09-22

**Linux ARM64 sysinfo is implemented with an explicit guest resource policy.
Redis proceeds past syscall 179 and now stops at epoll_create1, syscall 20.
The service is not ready: no PING/SET/GET or persistence result passed.**

## Implemented behavior

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
| swap, high memory, shared-memory pool, buffer cache | Zero: these guest facilities are not supplied by this runtime |
| load averages | Zero placeholder; load-average accounting is not implemented |

Memory accounting includes the runtime's retained arenas and allocation headers.
It excludes Unicorn's internal allocations and is **not host RAM, host RSS, or
guaranteed allocatable space for any single guest allocation**. Existing ELF,
heap and mmap limits still apply. Returning these guest-policy values is not a
claim of complete Linux system-statistics equivalence or whole-product memory
enforcement. A guest that requires accurate load averages is not qualified.

ABI references: [Linux UAPI structure](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/sysinfo.h)
and [sysinfo manual](https://man7.org/linux/man-pages/man2/sysinfo.2.html).

## Database result

The unchanged, hash-verified Redis 7.4.2 fixture still uses its documented
unsupported-MADV_FREE fallback. It now completes sysinfo, loads its configuration,
and initializes its monotonic clock. It then calls epoll_create1 and exits 125
because that syscall is not implemented. Redis also reports its fallback for an
unsupported NOFILE limit query; that warning is recorded, not suppressed.

The acceptance harness still requires real PING/SET/GET replies, successful
shutdown, a new runtime process and an AOF-backed GET after restart. The final
result remains `service_qualified: false`, with zero successful queries.
There is no Docker or host-native database fallback. Dockiva was not rebuilt.

## Validation

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

## Next implementation target

Implement epoll as an interface: creation/close, descriptor registration and
modification, readiness waiting, timeouts and invalid-descriptor behavior. Check
those semantics against Linux and rerun this same Redis acceptance. Creating an
epoll descriptor alone would not provide event-loop support. Threading and futex
waiting remain later known requirements; PostgreSQL remains unqualified.
