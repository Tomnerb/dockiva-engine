# M14 performance checkpoint

The harness measures startup-to-first-response, HTTP throughput, median latency
and p95 latency for explicit launcher argument arrays. It validates the response
body on every request. It does not invoke a shell for the configured commands.

Recorded here: three Linux-host interpreter samples, each with 50 requests and
four concurrent clients. See `evidence/m14-linux-runtime-only.json`. This is not a
Mac benchmark and not a comparison with a Linux VM. No performance advantage is
claimed. CPU/RSS and whole-VM resource accounting are not measured by this harness;
the full M14 acceptance gate remains OPEN.

## Same-Mac comparison setup

Use the exact `examples/m11_17/server.elf` and HTML fixture in both backends.
The supplied scratch Dockerfile packages that ELF without rebuilding it. Its CMD
uses `18080 0 --bind-any` to listen on all container interfaces, which Docker
published ports require. The native benchmark omits that option and stays on
loopback. The runtime still rejects wildcard binds. Rebuild the Docker image
after updating the guest; use a fresh private instance for the same guest bytes.

On your Mac, build the comparison image against your existing Linux-VM-backed
Docker engine, explicitly selecting its context (replace the example context):

```sh
docker --context desktop-linux build --platform linux/arm64 \
  -t vmfree-benchmark:local examples/m11_17
```

For the VM-free command, first create a stopped private instance so its rootfs
can be used for benchmarking:

```sh
python3 containers.py create bench-root examples/m11_17/http-image --read-only
```

Create `benchmark-config.json` using your real absolute project/rootfs paths:

```json
[
  {
    "name": "VM-free",
    "kind": "vmfree",
    "command": ["/ABS/PROJECT/build/vmfree-c", "run", "/ABS/HOME/.vmfree/containers/bench-root/rootfs/app/server", "--root", "/ABS/HOME/.vmfree/containers/bench-root/rootfs", "--max-steps", "10000000", "--", "0"]
  },
  {
    "name": "Linux VM through Docker",
    "kind": "linux-vm",
    "port": 18090,
    "command": ["docker", "--context", "desktop-linux", "run", "--rm", "--name", "vmfree-bench-vm", "--platform", "linux/arm64", "-p", "127.0.0.1:18090:18080", "vmfree-benchmark:local"],
    "cleanup": ["docker", "--context", "desktop-linux", "rm", "-f", "vmfree-bench-vm"]
  }
]
```

The named Docker container is created by this benchmark and removed afterwards.
Make sure the chosen name and host port are unused. The explicit external port
accounts for Docker's port forwarding; the guest's printed port is internal to
its container. Keep the comparison VM running before the run to measure warm-VM
container startup; measure cold VM startup separately and label it clearly.

```sh
python3 scripts/benchmark_m14.py --config benchmark-config.json \
  --samples 5 --requests 100 --concurrency 4 --report mac-m14-comparison.json
```

Repeat under comparable thermal/load conditions. The launcher overhead is part
of startup. HTTP timings include both execution and the chosen host networking
path. Later collection must separately report guest/runtime CPU, process RSS,
Docker container memory and whole-VM overhead; these counters are not interchangeable.
A backend result without its paired same-Mac baseline cannot establish an advantage.

## Updated Mac evidence and next measurement

See [M14-MATCHED-PROFILE.md](M14-MATCHED-PROFILE.md) for the validated sustained
Mac results, matched-rate collection and opt-in interpreter counters. Sustained
Mac performance and process/container resource collection are now evidenced;
whole-VM accounting remains open. Earlier Linux-only notes above are historical.

The next optimization checkpoint is documented in [M14-LOOKUP-CACHE.md](M14-LOOKUP-CACHE.md), with a same-binary cache on/off comparison.

The instruction-classification cache candidate is documented in [M14-DECODE-CACHE.md](M14-DECODE-CACHE.md). Its Mac performance validation is pending.
