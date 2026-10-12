# M14 synchronized host-process measurement

The uploaded 60-second idle measurement succeeded for PID 47448 (OrbStack
Helper), with 117 samples, 0.33384% CPU on a one-core scale, median RSS 512.40625
MiB, and RSS range 512.109375–528.515625 MiB. This is much steadier than the prior
30-second window (507.1–749.8 MiB), although RSS still declined during this run.
It is a selected-process baseline, not proof of all VM memory usage.

This benchmark-only update adds --host-vm-pid. It samples the selected host PID
only during each Docker measured load window, after warmup. Native and container
resource collection are retained. All collectors use the same load end timestamp;
late collector responses are excluded even if stopping another collector takes
time. Identity (start time plus executable name) is checked before and after each
Docker sample to detect restarts/PID reuse. Collection errors are explicit.

After extracting the patch, no native or Docker rebuild is required:

```sh
python3 scripts/benchmark_m14_sustained.py \
  --config benchmark-config-bind-fixed.json \
  --samples 4 --duration 15 --warmup 2 --concurrency 4 \
  --rate 3000 --resources --host-vm-pid 47448 \
  --report mac-m14-host-loaded.json
```

If OrbStack restarted, first run `python3 scripts/measure_vm_process.py --list`
and substitute its current Helper PID. Keep unrelated VM/container workloads
paused. Upload the resulting JSON. The script does not signal, stop or modify
the selected host process; normal benchmark container cleanup remains unchanged.

Each Docker sample includes `host_vm_process` with raw timestamps, RSS, cumulative
CPU, sampled peak RSS and sampled CPU percent. The top-level
`host_process_collection_completed` requires successful collection for all Docker
samples. HTTP completion, achieved target rate and container resource collection
remain separate statuses. Sampling overhead is part of this instrumented run;
use the earlier uninstrumented report for performance comparisons.

Interpret the host-process data alongside the idle baseline, native process and
container data. Do not add container CPU/memory to host helper usage: scopes may
overlap. Host helper RSS may exclude guest-backed memory or other helper processes.
A before/after RSS difference alone is not an allocation measurement. The full
whole-VM accounting gate remains open unless these scopes are independently
established.

Validation: existing benchmark tests plus real local HTTP load with controlled ps
outputs, host PID routing, and rejection of changed process identity before
container launch. Real Mac process metrics remain to be validated by this run.
