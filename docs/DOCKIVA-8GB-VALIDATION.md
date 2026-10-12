# Dockiva: 8 GB target and initial capacity validation

8 GB machines are a primary product target. Initial engineering targets are at
most 300 MiB for the idle Dockiva UI plus engine and helper processes, less than
1% idle CPU on the one-core scale, and ordinary UI responses within 100 ms.
These targets are not yet accepted results. Guest workloads require a separate
memory allowance; bounded guest allocation is not total host process memory.

## Measured on 2026-09-21

Host: Apple Silicon, macOS 26.5.1, **32 GiB RAM**. This is not an 8 GB hardware
qualification and no Windows or WSL2 comparison was performed.

| Isolated workload | Processes | Median summed RSS | Average CPU |
| --- | ---: | ---: | ---: |
| Bundled API engine, no containers | 1 | 44.3 MiB | 0.09% |
| Engine + four idle HTTP servers and their supervisors | 9 | 189.8 MiB | 0.38% |

Each resource window contains eleven snapshots over approximately ten seconds.
CPU is the delta of `ps` cumulative process CPU time divided by elapsed wall time;
100% represents one fully used core. The process sets remained stable. CPU time
has limited precision, so these are preliminary short-window observations.

RSS sums include the complete isolated engine/supervisor/interpreter process set,
including supervisors reparented after launch. Shared pages may be counted more
than once. RSS is not physical footprint. **Dockiva's GUI and WebViews are excluded**;
the 300 MiB whole-product target therefore remains unverified. Two Dockiva app
copies were open during inspection, and WebKit helpers were reparented to launchd,
so parent-process attribution could not establish a trustworthy GUI total.

One hundred HTTP requests dispatched through four workers to four containers
passed body/status validation: median 0.47 ms, p95 0.74 ms, maximum 4.60 ms.
These are tiny loopback fixture results, not representative application latency,
GUI responsiveness or sustained throughput measurements.

## Functional acceptance exercised

Twenty checks passed, including four concurrent running containers, rejection of
a fifth active runtime, reuse of a released slot, all four workloads surviving
API shutdown, token rotation and container rediscovery after API restart, and
Stop/Restart control through recovered supervisors. All test-owned containers and
processes were cleaned up. Existing user containers were not modified.

API recovery does not test an OS reboot, guest process crash, UI interaction,
corrupted storage recovery or long-duration reliability. Those remain separate
checks.

Reproduce with the bundled helper:

```sh
python3 scripts/verify_dockiva_capacity.py \
  --helper /path/to/Dockiva.app/Contents/Resources/vmfree-engine/vmfree-engine \
  --report evidence/dockiva-capacity.json
```

Evidence: [raw report](../evidence/dockiva-capacity.json) and
[verification output](../evidence/dockiva-capacity.txt).

## Remaining 8 GB gates

Measure one Dockiva instance and its attributable WebViews plus the engine, using
physical-footprint accounting and longer idle windows. Run on an actual 8 GB
Apple Silicon Mac, observing memory pressure and swap with a representative
supported stack. Verify UI latency, foreground/background behavior, sustained
workloads and recovery. RAM/CPU/battery savings versus Docker Desktop or WSL2
remain unproven; production milestone acceptance is unchanged.

## Go control-plane checkpoint — 2026-09-22

The [Go migration](DOCKIVAD-GO-MIGRATION-2026-09-22.md) reran the same capacity
harness for ten-second windows on the 32 GiB Mac. Idle API: 152.2 MiB summed
RSS across one process, 0.00% of one core. API plus four HTTP runtimes: 217.5 MiB
across 13 processes (including four C liveness guards), 0.84% CPU. All 20 functional
checks passed; [raw evidence](../evidence/dockivad-go/capacity.json).

No memory improvement is demonstrated by these RSS results: they are higher
than the earlier Python bundle's values. Returning temporary OCI allocations to
the OS did not reduce measured RSS. Physical footprint, reusable/shared pages,
whole-product UI memory and actual 8 GB hardware still need qualification. This
measurement must not be presented as Docker Desktop RAM/CPU/battery savings.
