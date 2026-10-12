# VM-free runtime — bounded musl application candidate

Run supported Linux ARM64 guests on Apple Silicon macOS without a Linux kernel VM.
The prototype supports bounded guest processes, noninteractive commands, buffered
pipes and file redirection. Earlier persistent HTTP networking, registry pulls,
Docker API and Compose subsets remain available.

Sustained same-Mac HTTP comparisons and both cache optimizations have been measured.
Whole-VM accounting, production security and signed clean-Mac distribution remain open. This is a development prototype, not general Docker compatibility or
a production isolation boundary. The default CPU backend remains interpreted.
**2026-10-12:** the Unicorn adapter has been removed. Its role is taken by the native
backend (`build/vmfree-c-hvf`, `--cpu-backend hvf`): guest code runs on the host CPU under
Hypervisor.framework with no guest kernel, and all Linux syscalls go through this runtime.
`scripts/verify_hvf.py` and `scripts/verify_fork_sockets.py` cover it. The notes below
describe the project's history.

The [opt-in Unicorn CPU adapter](docs/REDIS-UNICORN-INTEGRATION.md) passed
[prepared Redis query and persistence acceptance](docs/REDIS-QUERY-PERSISTENCE-2026-09-22.md)
and [four/eight-client concurrency and process-crash recovery tests](docs/REDIS-CONCURRENCY-RECOVERY-2026-09-22.md).
This is bounded prepared-ELF validation, not stock-image or production qualification.
The [local Dockiva Redis test build](docs/DOCKIVA-REDIS-PREVIEW-2026-09-22.md) now packages
the adapter and prepared Redis preset; its adapter lifecycle and persistence checks pass.

The [production roadmap](docs/PRODUCTION-ROADMAP.md) defines proposed M22–M80
work and the gates for a supported Docker replacement on Apple Silicon.
The [M1–M80 status ledger](docs/MILESTONE-STATUS.md) separates implemented scope
from open acceptance gates.
The [frozen architecture v1.0](docs/FINAL-ARCHITECTURE-V1.md) defines the production
target: SwiftUI/AppKit UI, Go dockivad and C runtime, with no Python in production
execution. [Component contracts](docs/TARGET-ARCHITECTURE-CONTRACTS.md) define
ownership, interfaces and implementation slices. [Valkey V11 and PostgreSQL P12](docs/WORKLOAD-GATES-V1.md)
are the major compatibility gates. These target components are not yet all implemented.
The [first CPU-state extraction](docs/CPU-STATE-OWNERSHIP-2026-09-22.md) passes
interpreter/Unicorn regressions, Redis recovery and bundled Dockiva adapter checks.
The subsequent [address-space extraction](docs/ADDRESS-SPACE-OWNERSHIP-2026-09-22.md)
adds explicit memory state and revision-driven backend mapping synchronization;
its mapping, recovery and packaged-adapter tests also pass.
[Reference-counted memory backing](docs/MEMORY-BACKING-OWNERSHIP-2026-09-22.md) now
keeps split mappings on the same allocation and releases it after the last owner.
[macOS host-page COW and bounded Unicorn fork](docs/PAGE-COW-UNICORN-FORK-2026-09-23.md) now
support private processes, inherited descriptors, nested forks and fork from a
multithreaded parent. Prepared Redis BGSAVE/AOF rewrite tests include parent
writes during child execution and validated restart data. Guest pages remain
4 KiB; physical COW uses 16 KiB host pages on the tested Mac. MAP_SHARED and
PostgreSQL shared-state workflows remain open.
The bundled control plane now uses [Go dockivad](docs/DOCKIVAD-GO-MIGRATION-2026-09-22.md) for the supported preview API, OCI preparation and supervisors. Its Go/C process tree and bundle contain no Python runtime.

[Versioned execution profiles](docs/COMPATIBILITY-PROFILES-2026-09-22.md) now select
backends by image digest, negotiate capabilities with the native runtime and expose
the effective policy in Inspect. All compatibility results remain experimental.

## Implementation toward M58

Development work now includes bounded larger ELF/mmap options, additional ARM64
instructions, glibc startup metadata and filesystem sync prerequisites. The direct
PostgreSQL version probe matches Linux. Earlier interpreter server probes recorded
bootstrap, virtual-device, process-capacity and floating-point instruction failures.
Prepared Redis now passes the opt-in Unicorn tests linked above; PostgreSQL and
Valkey services, broader database compatibility and M58 remain unaccepted.
See [database probe history](docs/DATABASE-COMPATIBILITY-PROBES.md)
and [the milestone ledger](docs/MILESTONE-STATUS.md).

## Current implementation work

M24–M26 add unmodified BusyBox ash, scoped job control and scheduled loopback
networking. M27 working directories, M28 bounded musl loading, M29 HTTP fetching,
M30 differential replay and parts of M31 resource/confinement work are now present.
Remaining acceptance gaps are recorded in the status ledger; M80 is not complete.

```sh
python3 scripts/build_native.py
python3 oci.py run examples/m24/shell-image --process-tree --max-steps 10000000 -- -c 'echo hello | cat'
python3 scripts/verify_m28.py --report mac-m28-validation.json
python3 scripts/verify_m30.py --report mac-m30-validation.json
```

The optional macOS `--host-sandbox` candidate supports loopback clients only;
server workloads cannot bind listeners in that mode. See [M31 scope](docs/MILESTONE-31.md).

## M23: foreground input

```sh
python3 scripts/build_native.py
python3 scripts/verify_m23.py --report mac-m23-validation.json
python3 oci.py run examples/m23/shell-image --interactive --max-steps 10000000 -- -c 'busybox cat'
```

Input, Ctrl-D and Ctrl-C are forwarded to scheduled guests. Host terminal modes
remain unchanged. See [M23 scope and validation](docs/MILESTONE-23.md).

## M22: scheduled guest cancellation

```sh
python3 scripts/build_native.py
python3 scripts/verify_m22.py --report mac-m22-validation.json
python3 scripts/verify_m22_native.py --sanitize
python3 oci.py run examples/m22/signal-image --schedule --max-steps 10000000 -- cancel-read
python3 oci.py run examples/m22/shell-image --schedule -- -c 'probe self-term; status'
```

Individual CPU-bound or pipe-blocked guests can be terminated with supported
guest signals; wait statuses and shell status preserve the cause. See
[M22 scope and evidence](docs/MILESTONE-22.md). Full signal handlers, terminal
input and production application qualification remain open.

## M21: concurrent pipeline streaming

```sh
python3 scripts/build_native.py
python3 scripts/verify_m21_native.py
python3 scripts/verify_m21.py --lifecycle --report mac-m21-validation.json
python3 oci.py run examples/m21/shell-image --schedule --max-steps 100000000 -- -c 'busybox echo hello | busybox cat'
```

The opt-in scheduler supports up to four live children. Readers and writers wait
when pipes are empty or full, allowing streams beyond the 1 MiB buffer capacity.
Try `probe large | busybox cat` to stream 4 MiB. All stages run and are waited for;
a nonzero stage status makes the pipeline fail. Interactive terminals remain open.
See [M21 scope and validation](docs/MILESTONE-21.md).

## M20: pipes and redirection

```sh
python3 scripts/build_native.py
python3 scripts/verify_m20_native.py
python3 scripts/verify_m20.py --lifecycle --report mac-m20-validation.json
python3 oci.py run examples/m20/shell-image -- -c 'busybox echo hello | busybox cat'
python3 oci.py run examples/m20/shell-image --writable -- -c 'busybox echo saved >/out; busybox cat </out'
```

Pipeline stages run sequentially with a 1 MiB buffer and stop on the first failed
stage. Redirection supports `<`, `>`, `>>` and `2>&1`; temporary OCI writes are
discarded after exit. See [M20 scope and validation](docs/MILESTONE-20.md).
Concurrent streaming and interactive terminals remain separate work.

## M19: bounded processes and noninteractive commands

M19 adds synchronous interpreted children, rooted static-ELF exec, wait statuses,
and a small guest shell with `;`, `&&`, `||`, quoting and exit handling.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m19_native.py
python3 scripts/verify_m19.py --lifecycle --report mac-m19-validation.json
python3 oci.py run examples/m19/shell-image -- -c 'busybox echo hello; probe exit 7; status'
```

See [M19 scope and validation](docs/MILESTONE-19.md). Children share a bounded
instruction budget; nested children, pipes and interactive terminals remain
unsupported. This is a small custom shell, not BusyBox ash compatibility.

## M18: selected upstream BusyBox commands

The new fixture runs `echo`, file-based `cat`, `pwd`, `ls`, and `mkdir` from a
minimal unmodified-source BusyBox build. Guest ELF and OCI image are included.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m18_native.py
python3 scripts/verify_m18.py --report mac-m18-validation.json
python3 oci.py run examples/m18/busybox-image -- echo 'Hello from BusyBox'
```

See [M18 scope, source, and validation](docs/MILESTONE-18.md). Shell/process/thread
support is still outside this milestone. Mac M18 validation remains pending.

## Earlier milestone validation on your Mac

Extract the full package into a fresh directory. Requires Python 3.10+ and `cc`.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m11_17.py --report mac-m11-17-validation.json
python3 scripts/verify_m7_10.py
```

Expect **30 new PASS lines**, then 40 earlier checks. No Docker installation is
required for these default tests. Optional real Docker/Compose validation adds
15 checks; see the [release guide](docs/MILESTONE-11-17.md).

## Run the two-service demo

Terminal 1:

```sh
python3 vmfree.py image import vmfree/server:demo examples/m11_17/http-image
python3 vmfree.py engine
```

Terminal 2:

```sh
python3 vmfree.py compose -f examples/m11_17/compose.json up
curl --noproxy '*' --max-time 10 http://127.0.0.1:18081/
python3 vmfree.py compose -f examples/m11_17/compose.json ps
python3 vmfree.py compose -f examples/m11_17/compose.json stop
```

The frontend on port 18081 connects to the backend on 18082. The response contains
`Persistent VM-free HTTP server OK`. Stop/up retains private container data;
`down` removes the instances and their data. Named volumes are not implemented.

If the Docker CLI is installed, try:

```sh
python3 vmfree.py docker version
python3 vmfree.py docker ps -a
python3 vmfree.py docker images
```

This targets the VM-free engine without changing your default Docker context.
No Docker Desktop is required. Only the documented API/Compose subset is supported.

## Results and next gates

Linux validation: 30 default checks, nine real Docker CLI 27.5.1 checks, six real
Docker Compose 2.32.4 checks and one QEMU network reference passed. Native sanitizer
checks and all earlier M2–M10 suites passed. M0–M10 Mac acceptance passed per user
report; this release's Mac acceptance is pending.

- [Release guide and commands](docs/MILESTONE-11-17.md)
- [Security review: open isolation gate](docs/SECURITY-REVIEW-M13.md)
- [Benchmark: same-Mac VM comparison pending](docs/BENCHMARK-M14.md)
- [Mac distribution tooling and pending gates](docs/MAC-DISTRIBUTION-M17.md)
- [Roadmap](docs/ROADMAP.md), [validation evidence](VALIDATION.md)
- [Earlier M7–M10](docs/MILESTONE-7-10.md)

No performance advantage over Docker or a Linux VM has been established. The
Mac packaging scripts must run on your Mac. The historical CLI package requires
Python for development commands. The new Dockiva helper uses Go/C only; Python
remains build and verification tooling.

## Dockiva integration for internal testing

Dockiva defaults to the bundled experimental VM-free engine on new Apple Silicon installations and starts it automatically, with no terminal setup. Existing engine choices are preserved. Supported images and basic lifecycle workflows remain the scope. See [project definition and setup](docs/PROJECT-DEFINITION.md). This integration does not close M33 preview or M80 production acceptance gates.
