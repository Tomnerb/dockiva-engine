# VM-free vs OrbStack (2026-10-14)

MacBook Pro M2 Max (32 GB), macOS 26.5. Medians of 3 runs (5 for container start);
the same images on both; VM-free is the Dockiva engine at this commit with its
defaults (2 GiB per container), OrbStack with its own. Memory is the resident size
(ps RSS) of each engine's host processes, the VM's included.

    python3 bench.py  vmfree|orbstack OUT.json   # containers, files, pipes, databases, builds
    python3 bench2.py vmfree|orbstack OUT.json   # go build (in /src) and a pure CPU loop
    python3 bench3.py vmfree|orbstack OUT.json   # engine cold start, memory, host -> container network

| Case | VM-free | OrbStack | VM-free |
|---|---|---|---|
| run alpine true | 0.131 s | 0.25 s | 48% less time |
| dd 1M x 1-byte | 4.79 s | 0.48 s | 10.0x the time |
| pgbench TPC-B tps | 4309.92 tps | 1894.919 tps | 2.3x more |
| npm install express+lodash | 5.705 s | 3.37 s | 1.7x the time |
| go build net/http (cold cache) | 7.496 s | 3.636 s | 2.1x the time |
| bind mount 2000 small files | 0.54 s | 0.513 s | 1.1x the time |
| bind mount 256 MiB write | 0.533 s | 0.67 s | 20% less time |
| container fs 256 MiB write | 0.5 s | 0.597 s | 16% less time |
| sha256 512 MiB (CPU) | 3.554 s | 4.404 s | 19% less time |
| awk loop 30M (CPU) | 9.375 s | 9.064 s | 1.0x the time |
| engine cold start to ready | 0.308 s | 4.426 s | 93% less time |
| engine cold start to first container | 0.536 s | 4.866 s | 89% less time |
| memory idle engine | 111.625 MiB | 2512.797 MiB | 96% less memory |
| memory with 5 idle containers | 189.469 MiB | 2102.328 MiB | 91% less memory |
| host->nginx keep-alive HTTP req/s | 7081.887 req/s | 8708.199 req/s | 19% fewer |
| host->redis PING round trips/s | 37551.487 ops/s | 16309.385 ops/s | 2.3x more |

Where VM-free leads: everything about the engine and containers' lifecycle (no VM
to boot or to hold memory), host -> container connections (host sockets, no VM
forwarding), file writes and pipes.

Where it trails: system-call-bound and heavily threaded work. Each system call is
a hypervisor exit (about 0.9 us, against about 0.1-0.2 us inside a Linux VM), and a
process's threads share locks the runtime takes per call: one-byte dd, go build,
npm install and nginx's per-request open/stat/close.

pgbench: VM-free's fsync is macOS fsync(2), which does not flush the drive's cache
(F_FULLFSYNC would); Linux's fsync is durable. Its higher TPS partly reflects that.
