# Project definition and Dockiva relationship

vmfree-runtime is an experimental ARM64 Linux user-mode compatibility runtime
for Apple Silicon macOS. It executes bounded guest programs using the default
interpreter or experimental Unicorn backend and translates a
subset of Linux system calls to macOS operations, without a Linux VM. Its eventual
objective is a production-qualified Docker replacement for an explicitly defined
workload corpus. It is not currently a general Docker replacement or an accepted
production isolation boundary.

Dockiva is the container workspace and user interface. It retains its existing
native Linux VM and external Docker backends, and adds a separate, opt-in VM-free
backend for internal integration testing (the default on new Apple Silicon installs).
Dockiva does not turn unsupported Linux
programs into supported workloads. The projects have independent milestone numbers;
Dockiva integration does not satisfy this runtime's M33 or M80 acceptance gates.

The [frozen architecture v1.0](FINAL-ARCHITECTURE-V1.md) is the production baseline.
Its [component contracts](TARGET-ARCHITECTURE-CONTRACTS.md) specify a Go
control plane, shared Linux state services, replaceable execution backends,
measured resource management and an explicit host isolation boundary. These are
implementation contracts, not a description of completed components. SwiftUI and
AppKit are the required production macOS UI; the existing Vue/Wails application
is the transitional development desktop. The production target uses Go dockivad
and the C runtime, with Python retained only for development tooling. The current
Go/C helper now implements that language boundary for the supported preview engine;
see [migration evidence](DOCKIVAD-GO-MIGRATION-2026-09-22.md). Valkey
persistence (V0–V11) is the first major compatibility gate, followed by PostgreSQL
crash recovery and stress (P0–P12); see [workload gates](WORKLOAD-GATES-V1.md).
Current Redis evidence remains regression protection, not acceptance of either gate.
CPU-state and [address-space state/synchronization](ADDRESS-SPACE-OWNERSHIP-2026-09-22.md)
extractions are implemented and regression-tested. [Reference-counted backing](MEMORY-BACKING-OWNERSHIP-2026-09-22.md)
now owns segment views, heap and mmap storage. [Private interpreter fork COW](PRIVATE-FORK-COW-2026-09-22.md)
defers allocation copies until writable access, preserving local segment views.
[Unicorn write-fault COW](UNICORN-COW-FAULTS-2026-09-22.md) now defers backend
backing copies until guest stores. [macOS host-page COW and bounded Unicorn fork](PAGE-COW-UNICORN-FORK-2026-09-23.md)
now support nested private processes, fork from a threaded parent, inherited
sockets/epoll, wait and single-threaded exec. Prepared Redis background snapshots
and AOF rewrites pass with parent writes and restart validation. MAP_SHARED and
PostgreSQL shared-state workflows remain unfinished. This checkpoint provides a
separate signed engine bundle, not an updated installed desktop app.

## Default engine and automatic startup

New Dockiva installations on Apple Silicon default to VM-free. Opening the app
starts the bundled engine when Start engine on launch is enabled (the default).
Selecting VM-free also starts it. Existing saved runtime choices and a disabled
startup preference are preserved. The new app bundles Go dockivad, C worker guards,
both native backends and three prepared demos; end users need no terminal setup.

The authenticated Unix socket and state use `~/.vmfree` (or a developer-supplied
`VMFREE_HOME`). Engine instances are reused; the backend stays VM-free on failure
rather than falling back. The engine and containers survive closing the UI.

Create `vmfree/shell:demo` with command JSON `["-c", "busybox echo hello"]` and no
ports. The shell/HTTP contract is read-only creation, image listing, list/inspect/start/stop/
restart/remove and snapshot logs. General pulls, builds, attach, volumes, Compose,
DNS, bridge networking, port publishing, migration, image deletion and resource
statistics remain unsupported. M24+ execution modes are not enabled automatically.
The separate prepared Redis preset below permits its private writable root.
[Digest-bound profiles](COMPATIBILITY-PROFILES-2026-09-22.md) select its Unicorn
backend independently of the image tag, validate native capabilities before
launch, and expose the saved policy through Inspect.

## Build and validation

The developer uses `python3 scripts/bundle_dockiva_engine.py --output /path/to/dockiva/bin`
with Go 1.22+, Xcode Command Line Tools and the prepared Unicorn static library,
then packages Dockiva. Python runs the build scripts only. PyInstaller and CPython
are absent from the helper. The package signs and verifies each executable;
Dockiva packaging rejects Python files and replaces the entire generated helper
resource tree to prevent stale files from older bundles.

The bundled startup test removes Python/Homebrew from PATH and exercises engine
startup/reuse, demo discovery, guest execution and restart in temporary state.
Full Go tests and the frontend build provide engineering checks. GUI interaction,
older macOS versions, notarization and clean-Mac installation remain unqualified.

## Removing Python from the shipped engine

The supported preview control-plane migration is implemented in [dockivad](../dockivad/).
Go owns authenticated API handling, verified OCI preparation, image cache, lifecycle,
state, snapshot logs, admission and recovery. A small C guard applies host resource
limits and kills/reaps the native worker when its Go supervisor disappears.

Race-enabled Go tests, live/bundled Dockiva adapter tests and crash/migration tests
pass. A running legacy Python supervisor remains controllable; its next start uses
Go/C. The state/control format is also readable by the old tools for rollback.
Existing running engines are reused, so the separate Go test launcher selects fresh
state rather than silently replacing an active engine.

[Delivery slice E](TARGET-ARCHITECTURE-CONTRACTS.md) is still partially open:
SwiftUI/AppKit, Developer-ID distribution and clean-Mac qualification are separate
requirements. This migration does not complete general Docker features or workload
gates. Python development tools are retained but are neither bundled nor invoked
by the Go daemon or its worker tree.

## Release and measurement definitions

Internal integration testing is available before a controlled preview. A
controlled preview is a scoped release to selected testers after its acceptance
gates pass. Production replacement requires the roadmap's compatibility,
isolation, reliability, distribution, support and pilot evidence through M80.
The current status ledger remains authoritative.

RAM, CPU and battery savings versus Docker Desktop remain unproven. Later
comparisons must account for complete runtime/backend process trees on the same
Mac with equivalent workloads, repeatable conditions and uncertainty. Historical
OrbStack measurements must not be labeled Docker Desktop measurements.

## Persistent service test

Dockiva now includes an HTTP server demo preset in addition to Hello. The bundled engine seeds `vmfree/server:demo`; the preset uses command `["18080"]`, read-only storage and no port mappings. It serves http://127.0.0.1:18080/ and stays running subject to the existing execution limits. A port conflict requires choosing another listening port. The bundled test verifies HTTP content, Running status, Stop listener closure, Start and Restart.

## 8 GB product target

Dockiva targets 8 GB machines. Proposed targets and the initial four-container/recovery results are recorded in [8 GB validation](DOCKIVA-8GB-VALIDATION.md). Current engine-only measurements on a 32 GiB Mac do not qualify total UI memory, battery savings or an 8 GB machine.

## Prepared Redis local test — 2026-09-22

The separate [Dockiva Redis test build](DOCKIVA-REDIS-PREVIEW-2026-09-22.md) bundles the opt-in Unicorn adapter and a digest-pinned prepared Redis 7.4.2 image. Its preset uses a private writable root and loopback port 16379. Packaged adapter tests cover automatic engine startup, PING/SET/GET and recovery after Stop/Start and Restart. Other images retain the interpreter. This is local experimental integration; PostgreSQL, Valkey, stock-image compatibility, distribution and production gates remain open.

The newest isolated test build is `dockiva-profiles-test.app` in the Dockiva checkout.
Use `Launch Dockiva Profiles Test.command` to select a separate profile-test store.
