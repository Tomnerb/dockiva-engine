# VM-free vs OrbStack (2026-10-14)

MacBook Pro M2 Max, macOS 26. Medians of 3 runs (5 for container start); the
same images on both engines; VM-free is the Dockiva engine at this commit with
its defaults (2 GiB memory per container), OrbStack with its own.

    python3 bench.py vmfree|orbstack OUT.json    # most cases
    python3 bench2.py vmfree|orbstack OUT.json   # go build (in /src) and a pure CPU loop

| Case | VM-free | OrbStack | VM-free time relative to OrbStack |
|---|---|---|---|
| run alpine true | 0.483 s | 0.25 s | 1.93x |
| dd 1M x 1-byte | 4.672 s | 0.48 s | 9.73x |
| pgbench TPC-B tps | 4263.951 tps | 1894.919 tps | 0.44x |
| npm install express+lodash | 6.426 s | 3.37 s | 1.91x |
| go build net/http (cold cache) | 9.257 s | 3.636 s | 2.55x |
| bind mount 2000 small files | 0.665 s | 0.513 s | 1.30x |
| bind mount 256 MiB write | 0.77 s | 0.67 s | 1.15x |
| container fs 256 MiB write | 0.735 s | 0.597 s | 1.23x |
| sha256 512 MiB (CPU) | 8.974 s | 4.404 s | 2.04x |
| awk loop 30M (CPU) | 9.468 s | 9.064 s | 1.04x |

Notes
- pgbench: VM-free's fsync is macOS fsync(2), which does not flush the drive's
  cache (F_FULLFSYNC would); Linux's fsync is durable. The higher TPS partly
  reflects that, so it is not a like-for-like durability comparison.
- System calls cost about 10x more (dd of 1-byte blocks: ~2.3 us per call vs
  ~0.24 us); the pipe-bound sha256 case and npm install show it.
- Pure guest computation (the awk loop) runs at about the same speed.
