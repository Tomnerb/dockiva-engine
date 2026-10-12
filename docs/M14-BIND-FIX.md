# M14 published-port fix

The prior Docker benchmark guest listened only on container loopback. Published
ports require a listener reachable through the container network interface.
The updated Docker CMD is `18080 0 --bind-any`. Native execution defaults to
loopback and still rejects wildcard binds. Both backends use the updated ELF.

From the project root after extracting this patch:

```sh
docker --context orbstack build --platform linux/arm64 -t vmfree-benchmark:local examples/m11_17
python3 scripts/setup_m14_bind_fix.py
python3 scripts/benchmark_m14.py --config benchmark-config-bind-fixed.json --samples 5 --requests 100 --concurrency 4 --report mac-m14-comparison-bind-fixed.json
```

The setup script retains Docker command, port, and cleanup from your original
benchmark-config.json. It creates a new stopped, read-only private benchmark
instance and writes a separate config. Keep the published host address at
127.0.0.1. The old benchmark instances remain available.

Local validation: 30 M11–M17 checks; five readiness tests; QEMU Linux HTTP on
127.0.0.2 with wildcard binding; default loopback-only binding; native wildcard
rejection. macOS/OrbStack validation remains pending. No performance comparison
or resource savings are established by these checks.
