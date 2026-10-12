# M14 matched-rate and counter profiling update

## Validated Mac results

Uploaded sustained report: macOS 26.5.1 ARM64, four AB/BA rounds, 15-second load
windows, 2-second warmup and concurrency 4. All eight samples completed with
1,169,868 validated workload responses. Medians across per-run measurements:

| Metric | VM-free | Docker / OrbStack |
|---|---:|---:|
| Startup to first response | 10.05 ms | 372.42 ms |
| Throughput | 12,443 requests/s | 7,143 requests/s |
| Median latency | 0.307 ms | 0.524 ms |
| Per-run p95 latency | 0.319 ms | 0.773 ms |

The separate instrumented run also completed. Native sampled CPU was a median
94.84% of one core and sampled peak RSS was 2.53125 MiB in every run. Docker's
median-of-run-median CPU reading was 11.28%; reported memory ranged from 936 KiB
to 2.066 MiB. These are different accounting scopes and different achieved loads.
No CPU efficiency, whole-system memory saving, or energy advantage is established.
Startup includes direct native launch versus Docker container creation and
forwarding readiness, with an already-running VM. Supported guest compatibility
and production-security gates remain separate.

## Run the updated checks on Mac

Rebuild only the host runtime; Docker image and private guest roots stay valid:

```sh
python3 scripts/build_native.py
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 3000 --resources --report mac-m14-matched-resources.json
```

The total target is 3,000 requests/s, shared across four workers. The scheduler
spaces requests and drops stale slots instead of issuing catch-up bursts. Each
result includes achieved throughput and target fraction. `matched_rate_achieved`
requires every sample to deliver at least 95% of target; inspect individual
fractions before comparing CPU. This is a paced, bounded-concurrency generator,
not a full open-loop load tool; response latency excludes scheduling delay and
missed slots, and cannot describe overload tails. At very low rates, quantization
of slot counts matters. Resource polling adds overhead.

Run native counter collection separately to avoid adding its instrumentation to
the resource comparison:

```sh
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 2 --duration 10 --warmup 2 --concurrency 4 \
  --rate 3000 --profile --report mac-m14-matched-profile.json
```

`--profile` instruments only the native backend. It counts executed instructions,
permission-checked memory lookups, segment probes and syscall numbers below 512.
Counters are cumulative across startup, warmup, load and snapshot wait, not
load-window-only. A SIGUSR1 request produces a JSON snapshot on stderr. Normal
exit also emits counters. The signal handler only sets a flag; output occurs in
normal execution. Profiling interrupts of the guest network wait are handled as
a host instrumentation event, allowing the guest poll loop to continue. Snapshot
success is separate from HTTP success (`profile_collection_completed`).

These are event counters, not sampled CPU stacks or per-instruction timing. They
can identify frequent operations but cannot prove which operation costs the most
CPU. Instrumentation changes timing. Use the previous uninstrumented report for
the performance claim. Runtime defaults are preserved except for the benchmark's
already-documented explicit 10-billion instruction budget. The new runtime does
add disabled counter checks to its hot path; rerun uninstrumented timings before
claiming unchanged performance after this build.

## Host VM process measurement

List candidates without modifying or stopping any process:

```sh
python3 scripts/measure_vm_process.py --list
```

Send this output to identify the actual VM process. The list is heuristic and may
include helpers. Once the PID is identified, use `--pid` with `--duration` and
`--report` to record an idle baseline, then a separate window overlapping a
Docker-only load interval. Do not label a window containing alternating native
and Docker samples as Docker-only. Keep other VM workloads paused.

The helper records identity, RSS and CPU of the explicitly selected host PID.
It does not automatically claim whole-VM accounting: virtualized guest memory,
shared mappings and helper processes may have different accounting. A selected
process measurement alone may not close the whole-VM gate. No processes are
killed or settings changed by this helper.

## Validation scope

Benchmark unit/integration tests cover pacing and existing resource/error paths.
Native Linux checks validate hello stdout, exact hello syscall counts, paced
HTTP and a live SIGUSR1 snapshot without server exit. The existing M2 and M11–17
regressions were also run. Actual macOS profiling/signal behavior and the new
matched-rate resource run remain to be validated by the commands above.
