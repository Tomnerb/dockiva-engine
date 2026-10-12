# M14 immutable-segment lookup cache

The runtime now keeps the most recent immutable ELF/stack segment for each
permission value. A cache hit checks the full access range and permissions
before returning a host pointer. Failed candidates use the original segment
scan and heap lookup. Heap and mmap mappings are never cached, so brk shrink,
unmap, reuse and protection checks always consult current state. Zero-length
accesses bypass the cache to preserve the original segment-boundary ordering.

This is a memory-lookup optimization, not a JIT or an OS isolation change.
`--no-lookup-cache` selects the reference scan path in the same binary. It must
appear before the guest argument separator `--`. Profiling adds
`lookup_cache_hits`; `segment_probes` now includes attempted cached-segment checks
as well as fallback scan checks. Counters count events, not CPU time.

## Mac validation and comparison

Extract into the existing project, then rebuild the native runtime only:

```sh
python3 scripts/build_native.py
python3 scripts/verify_lookup_cache.py
```

Compare process CPU at a matched target rate:

```sh
python3 scripts/benchmark_lookup_cache.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 3000 --resources --report mac-m14-cache-cpu.json
```

Compare capacity without resource polling:

```sh
python3 scripts/benchmark_lookup_cache.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 0 --report mac-m14-cache-throughput.json
```

Both reports compare cache off/on using the same native binary and guest, in
balanced AB/BA order. No Docker backend is launched. Configuration files and
images remain unchanged. The native instruction budget is 10 billion per sample.
Upload both reports. Do not run them simultaneously. The resource comparison is
instrumented; the throughput comparison is uninstrumented. Cache-off has the
reference semantics but shares the new binary layout and a disabled-cache branch;
it is not a byte-identical pre-change binary. `--profile` is available for separate
counter collection and should not be enabled in the primary performance runs.

## Local validation

- Direct cached/reference pointer equivalence across permissions, overflow-sized
  inputs, zero-byte and cross-segment boundaries, holes, brk shrink, mmap unmap
  and PROT_NONE reuse.
- Existing M2, M3, M4, M7–10 and M11–17 validation suites passed locally.
- Paced HTTP smoke with live profile snapshots: cache off/on both completed.
  Approximately 84% of lookups hit; segment probes fell about 39% in that run.
  Slightly different polling/scheduling can change total instruction counts.
- Paired runner smoke verified AB/BA order, both completion statuses and profiles.

Reduced lookup work is established locally. Mac CPU and throughput improvement
remain unproven until the two reports above are collected. No guest binaries,
Docker image, filesystem/network restrictions or process limits were changed.

## Prior Mac host-process evidence

At similar achieved rates (~2,938 native vs ~2,958 Docker requests/s), the previous
instrumented run reported median CPU 31.18% for the native process and 132.21% for
the selected OrbStack Helper. Median-of-run-median RSS was 2.53 MiB vs 706.14 MiB.
The 60-second Helper idle run showed 0.334% CPU and median RSS 512.41 MiB. These
are selected-process measurements and do not establish complete whole-VM resource
accounting. The current optimization should first be judged against its own
cache-disabled native baseline.
