# Redis startup-check investigation — 2026-09-22

Follow-up: [guest sysinfo is now implemented](REDIS-SYSINFO-2026-09-22.md).
The latest Redis stop is epoll_create1 (20); the syscall-179 stop below is the
earlier result. No service or fork/COW qualification follows from either batch.

**The original startup stop is resolved through Redis's own unsupported-feature
fallback. Redis is still not a working service. Fork/copy-on-write has not been
implemented or validated by this result.**

## What the source and trace showed

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

## Implementation

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

## Observed result

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

## Validation

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

## Next boundary

Implement guest-scoped sysinfo reporting, then rerun the same pinned Redis
acceptance to identify its next actual dependency. Keep the full service goal
as PING/SET/GET plus AOF persistence across restart. Do not promote database or
threading milestones based on this fallback result.
