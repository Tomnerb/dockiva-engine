# Dockiva Engine

Dockiva Engine runs Linux containers on Apple Silicon Macs with no Linux VM.
Container code runs directly on the Mac's CPU under Hypervisor.framework, and the
engine answers the Linux system calls itself. It is the default engine of
[Dockiva](https://github.com/Tomnerb/Dockiva), which bundles it as
`Dockiva.app/Contents/Resources/dockiva-engine/`. Its state lives in
`~/.dockiva-engine`.

## How it works

- **dockivad** (Go, `dockivad/`) serves the Docker Engine API. The Docker CLI,
  Compose and BuildKit reach it through the `dockiva` Docker context. It pulls and
  stores images and supervises containers.
- **dockiva-hvf** (C, `native/`) runs a container's processes. Guest ARM64 code runs
  on the CPU in a Hypervisor.framework VM with no guest kernel. Most system calls
  exit to the runtime on the Mac, which answers them. A few frequent ones are
  answered inside the guest at EL1, without leaving it.
- **Images** are unpacked once. Each container writes to its own upper layer, and
  the image's files stay unchanged.
- **linux/amd64** images run through FEX, described below.
- Where a VM cannot be created, such as inside a VM or on CI runners, the same
  runtime interprets the guest code instead (`dockiva-runtime`).

## Status

Dockiva uses this engine by default on new Apple Silicon installations and starts
it automatically. An engine a user chose before is kept
([how Dockiva starts it](docs/PROJECT-DEFINITION.md#default-engine-and-automatic-startup)).
Each Dockiva release bundles the engine commit pinned in its `DOCKIVA_ENGINE_REF`.

The engine is young. It implements the Linux system calls itself, so some
programs still need calls or behaviour it lacks. The independent security review
of the boundary between containers and the Mac is still open; see
[SECURITY.md](SECURITY.md). Report problems in
[Dockiva's issues](https://github.com/Tomnerb/Dockiva/issues).

Measurements against OrbStack on the same Mac are in
[Dockiva's README](https://github.com/Tomnerb/Dockiva#benchmarks).

## Build and test

On an Apple Silicon Mac with the Xcode Command Line Tools, Go 1.24 and Python 3
(3.14+, or the `zstd` command, for the FEX bundle):

```sh
python3 scripts/bundle_dockiva_engine.py --output build/dockiva-go   # the bundle Dockiva ships
go -C dockivad test -race -ldflags=-linkmode=external ./...
python3 scripts/verify_hvf.py
python3 scripts/verify_overlay.py
```

The bundle script builds the runtimes (`scripts/build_native.py`), dockivad and the
FEX bundle. The FEX bundle's first build downloads four pinned Ubuntu packages.
The other `scripts/verify_*.py` checks cover memory, processes, networking and the
Docker API, and [docs/DOCKIVA-INTEGRATION.md](docs/DOCKIVA-INTEGRATION.md) shows
how to run them against a bundle. CI builds the interpreter on Linux and replays
the compatibility corpus.

## linux/amd64 images

Images without an arm64 variant, or asked for with `--platform linux/amd64`, run
their x86-64 programs through [FEX](https://github.com/FEX-Emu/FEX), an x86-64
translator that is itself an arm64 Linux program (MIT). The runtime acts as
binfmt_misc with FEX registered: an exec of an x86-64 ELF starts FEX with the
program's descriptor (`AT_EXECFD`), and FEX and its arm64 libraries come from a
bundle mounted read-only at `/.dockiva/x86`. `docker pull`, `create` and `run` with
`--platform linux/amd64`, and `docker load` of amd64 archives, work.

`docker build --platform linux/amd64` (BuildKit or the classic builder) builds an
amd64 image: `RUN` steps go through FEX, `TARGETPLATFORM`/`TARGETARCH` say
linux/amd64, and `BUILDPLATFORM` stays linux/arm64, so `FROM --platform=$BUILDPLATFORM`
stages cross-compile natively. `FROM --platform=linux/amd64` works in any build. A
base image of another architecture than its local tag (amd64 alpine where `alpine`
is arm64) is pulled into the build cache, not the tag, as BuildKit leaves tags
alone; `docker builder prune` removes it. FEX's own files (its config,
FEXServer's lock and socket, telemetry) stay out of built and committed images. One
platform per build: the image store keeps one image per tag. `docker buildx ls`
lists linux/amd64 once the bundle is installed.

The bundle holds `bin/FEX`, `bin/FEXServer`, `lib/` with their arm64 glibc
(`ld-linux-aarch64.so.1`, `libc.so.6`, `libm.so.6`, `libgcc_s.so.1`,
`libstdc++.so.6`), and `share/` with the packages' copyright files and `SOURCES`
(each package, its SHA-256 and where its source is) and FEX's x86 vDSO
(`share/fex-emu/GuestThunks/libVDSO-guest.so`, `GuestThunks_32` for i386): the
runtime names it to FEX with `FEX_THUNKGUESTLIBS` in an x86 program's environment
(also after `env -i`), so `clock_gettime` and `gettimeofday` are not system
calls. `scripts/build_x86.py`
assembles it into `build/x86` from pinned packages, each checked against its size
and SHA-256: FEX 2610 from the fex-emu PPA (`fex-emu-armv8.4`, built for Ubuntu
24.04) and the libraries from Ubuntu 24.04's libc6 2.39-0ubuntu8.9 and gcc-14
14.2.0-4ubuntu2~24.04.1, the ones the ubuntu:24.04 image ships. Unpacking needs
zstd (Python 3.14+, or the `zstd` command). `scripts/bundle_dockiva_engine.py` puts
the bundle in the engine bundle at `_internal/build/x86` (about 10 MB), so the app
ships it. FEX's package is kept in `third_party/fex`, as Launchpad expires a PPA's
files once superseded; the Ubuntu packages are downloaded once into
`build/x86-cache`. The engine looks for the bundle at `$DOCKIVA_X86`, in an `x86`
directory beside the runtime binary, then in `~/.dockiva-engine/x86`; without one,
an amd64 container does not start and says where to put it. Tested with FEX 2610
and those libraries. On an M2 Max, Debian,
Alpine and Python 3.12 amd64 images pass shell, fork and exec, signal, thread,
multiprocessing, ssl, sqlite, subprocess and published-port HTTP checks; a Python
loop runs about 1.5x slower than on the arm64 image, and a container starts in
about 0.23 s (0.06 s for arm64). amd64 builds of Alpine (`apk add`), Debian and
Python (`pip install`) images, and a Go cross-compile with an amd64 final stage,
build and run with the classic builder and BuildKit.

## History

The rest of this file is the project's development log, kept as written. Each part
describes the project at that point. Since then, the native backend has become the
default, the Unicorn adapter has been removed, and the Python tooling, which keeps
the original development name (`vmfree.py`), is used only to build and test.

Run supported Linux ARM64 guests on Apple Silicon macOS without a Linux kernel VM.
The prototype supports bounded guest processes, noninteractive commands, buffered
pipes and file redirection. Earlier persistent HTTP networking, registry pulls,
Docker API and Compose subsets remain available.

Sustained same-Mac HTTP comparisons and both cache optimizations have been measured.
Whole-VM accounting, production security and signed clean-Mac distribution remain open. This is a development prototype, not general Docker compatibility or
a production isolation boundary. The default CPU backend remains interpreted.
**2026-10-12:** the Unicorn adapter has been removed. Its role is taken by the native
backend (`build/dockiva-hvf`, `--cpu-backend hvf`): guest code runs on the host CPU under
Hypervisor.framework with no guest kernel, and all Linux syscalls go through this runtime.
`scripts/verify_hvf.py` and `scripts/verify_fork_sockets.py` cover it. The notes below
describe the project's history.

The [opt-in Unicorn CPU adapter](docs/REDIS.md#redis-unicorn-integration) passed
[prepared Redis query and persistence acceptance](docs/REDIS.md#redis-query-persistence)
and [four/eight-client concurrency and process-crash recovery tests](docs/REDIS.md#redis-concurrency-recovery).
This is bounded prepared-ELF validation, not stock-image or production qualification.
The [local Dockiva Redis test build](docs/REDIS.md#dockiva-redis-preview) now packages
the adapter and prepared Redis preset; its adapter lifecycle and persistence checks pass.

The [production roadmap](docs/ROADMAP.md#production-roadmap) defines proposed M22–M80
work and the gates for a supported Docker replacement on Apple Silicon.
The [M1–M80 status ledger](docs/MILESTONE-STATUS.md) separates implemented scope
from open acceptance gates.
The [frozen architecture v1.0](docs/FINAL-ARCHITECTURE-V1.md) defines the production
target: SwiftUI/AppKit UI, Go dockivad and C runtime, with no Python in production
execution. [Component contracts](docs/TARGET-ARCHITECTURE-CONTRACTS.md) define
ownership, interfaces and implementation slices. [Valkey V11 and PostgreSQL P12](docs/WORKLOAD-GATES-V1.md)
are the major compatibility gates. These target components are not yet all implemented.
The [first CPU-state extraction](docs/MEMORY-AND-STATE.md#cpu-state-ownership) passes
interpreter/Unicorn regressions, Redis recovery and bundled Dockiva adapter checks.
The subsequent [address-space extraction](docs/MEMORY-AND-STATE.md#address-space-ownership)
adds explicit memory state and revision-driven backend mapping synchronization;
its mapping, recovery and packaged-adapter tests also pass.
[Reference-counted memory backing](docs/MEMORY-AND-STATE.md#memory-backing-ownership) now
keeps split mappings on the same allocation and releases it after the last owner.
[macOS host-page COW and bounded Unicorn fork](docs/UNICORN.md#page-cow-unicorn-fork) now
support private processes, inherited descriptors, nested forks and fork from a
multithreaded parent. Prepared Redis BGSAVE/AOF rewrite tests include parent
writes during child execution and validated restart data. Guest pages remain
4 KiB; physical COW uses 16 KiB host pages on the tested Mac. MAP_SHARED and
PostgreSQL shared-state workflows remain open.
The bundled control plane now uses [Go dockivad](docs/DOCKIVA-INTEGRATION.md#dockivad-go-migration) for the supported preview API, OCI preparation and supervisors. Its Go/C process tree and bundle contain no Python runtime.

[Versioned execution profiles](docs/DOCKIVA-INTEGRATION.md#compatibility-profiles) now select
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
server workloads cannot bind listeners in that mode. See [M31 scope](docs/MILESTONES.md#m31).

## M23: foreground input

```sh
python3 scripts/build_native.py
python3 scripts/verify_m23.py --report mac-m23-validation.json
python3 oci.py run examples/m23/shell-image --interactive --max-steps 10000000 -- -c 'busybox cat'
```

Input, Ctrl-D and Ctrl-C are forwarded to scheduled guests. Host terminal modes
remain unchanged. See [M23 scope and validation](docs/MILESTONES.md#m23).

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
[M22 scope and evidence](docs/MILESTONES.md#m22). Full signal handlers, terminal
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
See [M21 scope and validation](docs/MILESTONES.md#m21).

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
discarded after exit. See [M20 scope and validation](docs/MILESTONES.md#m20).
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

See [M19 scope and validation](docs/MILESTONES.md#m19). Children share a bounded
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

See [M18 scope, source, and validation](docs/MILESTONES.md#m18). Shell/process/thread
support is still outside this milestone. Mac M18 validation remains pending.

## Earlier milestone validation on your Mac

Extract the full package into a fresh directory. Requires Python 3.10+ and `cc`.

```sh
cd dockiva-engine
python3 scripts/build_native.py
python3 scripts/verify_m11_17.py --report mac-m11-17-validation.json
python3 scripts/verify_m7_10.py
```

Expect **30 new PASS lines**, then 40 earlier checks. No Docker installation is
required for these default tests. Optional real Docker/Compose validation adds
15 checks; see the [release guide](docs/MILESTONES.md#m11-17).

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

This targets Dockiva Engine without changing your default Docker context.
No Docker Desktop is required. Only the documented API/Compose subset is supported.

## Results and next gates

Linux validation: 30 default checks, nine real Docker CLI 27.5.1 checks, six real
Docker Compose 2.32.4 checks and one QEMU network reference passed. Native sanitizer
checks and all earlier M2–M10 suites passed. M0–M10 Mac acceptance passed per user
report; this release's Mac acceptance is pending.

- [Release guide and commands](docs/MILESTONES.md#m11-17)
- [Security review: open isolation gate](docs/MILESTONES.md#security-review-m13)
- [Benchmark: same-Mac VM comparison pending](docs/MILESTONES.md#m14-benchmark)
- [Mac distribution tooling and pending gates](docs/MILESTONES.md#mac-distribution-m17)
- [Roadmap](docs/ROADMAP.md#roadmap), [validation evidence](VALIDATION.md)
- [Earlier M7–M10](docs/MILESTONES.md#m7-10)

No performance advantage over Docker or a Linux VM had been established then;
[Status](#status) links current measurements. The Mac packaging scripts must run on
your Mac. The historical CLI package requires Python for development commands. The new Dockiva helper uses Go/C only; Python
remains build and verification tooling.

## License

Dockiva Engine is licensed under the [Apache License 2.0](LICENSE). Third-party
components and test fixtures, some under other licenses, are listed in
[third_party/NOTICE.md](third_party/NOTICE.md). Report vulnerabilities as
[SECURITY.md](SECURITY.md) describes.
