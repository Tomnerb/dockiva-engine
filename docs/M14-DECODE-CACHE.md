# M14 bounded instruction-classification cache

The interpreter now separates its 31 instruction-class predicates from execution
and dispatches through a standard C switch. A 1,024-entry direct-mapped table,
8 KiB on the tested build, caches the class keyed by the full 32-bit instruction
word. Hits avoid rescanning the classification predicates. Operands and immediates
are still decoded in the execution handlers; this is not a full decoded-block
cache, JIT, or host executable mapping.

Every iteration retains PC alignment, executable permission, full four-byte fetch
bounds and instruction-budget checks. The instruction word is fetched again
before consulting the cache. Modified bytes therefore select their current
classification; collisions compare the full word and replace the slot on a miss.
PC-relative operations continue to use the current PC in their execution handler.
All reserved-encoding and unsupported-instruction checks remain in place.

`--no-decode-cache` selects classification on every instruction in the same
switch-dispatch binary. `--no-lookup-cache` remains an independent memory-lookup
control. The decode benchmark leaves memory lookup caching as configured; the
existing benchmark config enables it in both variants.

## Mac commands

No guest or Docker image rebuild is needed. Rebuild the host runtime:

```sh
python3 scripts/build_native.py
python3 scripts/verify_decode_cache.py
```

Compare CPU at matched load (about 2–3 minutes):

```sh
python3 scripts/benchmark_decode_cache.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 3000 --resources --report mac-m14-decode-cpu.json
```

Compare unpaced capacity separately:

```sh
python3 scripts/benchmark_decode_cache.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 0 --report mac-m14-decode-throughput.json
```

These are native-only, same-binary AB/BA comparisons. Upload both JSON reports.
Do not enable `--profile` for primary performance measurements. Optional separate
profiling reports include `decode_checks` (ordered class predicates evaluated),
`decode_hits` and `decode_misses`. With caching disabled, hits/misses are zero and
all dispatches classify. Counters include startup, warmup and snapshot wait and
measure event frequency, not elapsed decoding CPU time.

Cache-off uses the new classifier and switch, not the pre-change if/else
interpreter. Thus this A/B test isolates caching within the new dispatcher;
comparison against a previous release additionally needs controlled measurements
of that earlier binary. Performance improvements are not established by hit rate
alone. The change remains a candidate until the Mac results are evaluated.

## Validation

- One million deterministic instruction words compared against the frozen original
  predicate ordering, repeated hits, collision churn and fixed special encodings.
- Execution with cache on/off validates modified instructions at the same address,
  PC-relative instructions at different addresses and executable permissions.
- Direct memory-lookup equivalence tests still pass.
- Existing M2, M3, M4, M7–10 and M11–17 checks passed locally after the dispatch
  refactor, including malformed/reserved instructions and memory faults.
- Existing benchmark tests and paired CLI profile smoke completed.
- Local paced HTTP smoke: approximately 98% classification hit rate; approximately
  98% fewer predicate checks. The report is bundled as Linux smoke evidence only.

## Previously validated memory-lookup result

The user's preceding same-binary Mac reports showed 13.7% lower matched-load CPU
(31.67% to 27.33%) and 16.1% higher unpaced throughput (9,848 to 11,436 requests/s)
with lookup caching. That cache stays enabled in this experiment. This does not
predict the additional benefit, if any, from classification caching.
