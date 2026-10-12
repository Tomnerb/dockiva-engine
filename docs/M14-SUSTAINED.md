# M14 sustained comparison

Prerequisite: the M14 bind fix is installed, its Docker image was rebuilt, and
benchmark-config-bind-fixed.json already completed on your Mac. No new image or
native build is required for this benchmark-only patch.

Keep OrbStack running, connect power, and avoid other heavy workloads. Both runs
use the existing configuration and Docker context. Each sample creates a fresh
backend. Four rounds alternate AB, BA, AB, BA, balancing which backend runs first.
Startup includes launcher and host HTTP readiness; this is not cold-VM startup.
The 2-second load warmup is excluded from throughput. Each worker sends sequential
requests until the 15-second deadline; pending requests finish and their drain
time is included. All responses retain the original workload content check.
Requests do not use keep-alive. Each latency percentile is for one sample.

## 1. Performance run (about 2–3 minutes)

```sh
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --report mac-m14-sustained.json
```

## 2. Resource run (about 2–3 minutes)

```sh
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --resources --report mac-m14-resources.json
```

Use the first report for performance comparisons. Resource polling has different
overhead for the two backends and perturbs timing. Upload both JSON reports.
Progress prints before and after every sample. A partial report is saved after
each completed sample and on caught errors. Keyboard interruption also saves
partial results and triggers backend cleanup. Existing cleanup commands are used.

## What resource numbers mean

- Native: `ps` samples direct runtime PID RSS and cumulative CPU time every 0.5s.
  Sampled CPU percent is CPU-time delta / wall-time delta, with 100% representing
  one core. Peak RSS is the largest sampled value, not a true lifetime peak.
  The launch command must directly start the runtime, as the supplied config does.
- Docker: `docker stats --no-stream` targets the configured container name through
  the same context. Raw CPU percent and memory usage/limit are retained. Collection
  is slower than native ps, and sample timestamps reflect command completion.
  The memory value is Docker CLI accounting, not process RSS. Docker documents
  that Linux CLI memory subtracts cache: https://docs.docker.com/reference/cli/docker/container/stats/
- These scopes are different. Do not divide their memory values to claim overall
  memory savings. No whole-VM CPU/RSS, Docker daemon overhead, client CPU, energy,
  or cold VM launch measurement is included. Whole-VM accounting remains open.
- `completed` means the HTTP runs succeeded. For instrumented reports,
  `resource_collection_completed` separately requires at least two samples per
  backend run and no collector errors. Missing access/tools are explicit errors,
  not zero usage. A failed collector does not discard valid HTTP measurements.
- The Python load generator and local networking can limit throughput. This is
  evidence for this HTTP fixture at this concurrency, not arbitrary containers.

The script records the effective command and raises only its native benchmark
instruction budget to 10 billion. Runtime defaults, managed-container limits,
existing config files, and guest binaries are unchanged. Duration accepts 1–60s;
sample count must be even, 2–20; concurrency 1–8. Very long workloads may still
exhaust the finite instruction budget; failures remain explicit.

## Validation included

11 benchmark tests pass, including real duration-based HTTP traffic, invalid
response failure, CPU clock parsing, guest-argument preservation, resource error
reporting, and Docker context retention. Resource parsing/collection tests use
controlled ps/Docker outputs. A three-second Linux native smoke run completed
12,128 validated requests. Local ps access was denied and correctly recorded as
incomplete. Real macOS ps and OrbStack stats collection require your Mac run;
no new macOS performance result is claimed in this patch.
