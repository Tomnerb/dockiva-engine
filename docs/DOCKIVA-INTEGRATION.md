# Dockiva integration

Running the runtime inside Dockiva: validation on an 8 GB Mac, the Go engine migration and compatibility profiles.

- [Dockiva: 8 GB target and initial capacity validation](#dockiva-8gb-validation)
- [Go dockivad migration — 2026-09-22](#dockivad-go-migration)
- [Digest-bound execution profiles — 2026-09-22](#compatibility-profiles)

<a id="dockiva-8gb-validation"></a>

## Dockiva: 8 GB target and initial capacity validation

8 GB machines are a primary product target. Initial engineering targets are at
most 300 MiB for the idle Dockiva UI plus engine and helper processes, less than
1% idle CPU on the one-core scale, and ordinary UI responses within 100 ms.
These targets are not yet accepted results. Guest workloads require a separate
memory allowance; bounded guest allocation is not total host process memory.

### Measured on 2026-09-21

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

### Functional acceptance exercised

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

### Remaining 8 GB gates

Measure one Dockiva instance and its attributable WebViews plus the engine, using
physical-footprint accounting and longer idle windows. Run on an actual 8 GB
Apple Silicon Mac, observing memory pressure and swap with a representative
supported stack. Verify UI latency, foreground/background behavior, sustained
workloads and recovery. RAM/CPU/battery savings versus Docker Desktop or WSL2
remain unproven; production milestone acceptance is unchanged.

### Go control-plane checkpoint — 2026-09-22

The [Go migration](#dockivad-go-migration) reran the same capacity
harness for ten-second windows on the 32 GiB Mac. Idle API: 152.2 MiB summed
RSS across one process, 0.00% of one core. API plus four HTTP runtimes: 217.5 MiB
across 13 processes (including four C liveness guards), 0.84% CPU. All 20 functional
checks passed; [raw evidence](../evidence/dockivad-go/capacity.json).

No memory improvement is demonstrated by these RSS results: they are higher
than the earlier Python bundle's values. Returning temporary OCI allocations to
the OS did not reduce measured RSS. Physical footprint, reusable/shared pages,
whole-product UI memory and actual 8 GB hardware still need qualification. This
measurement must not be presented as Docker Desktop RAM/CPU/battery savings.

<a id="dockivad-go-migration"></a>

## Go dockivad migration — 2026-09-22

The supported Dockiva preview control plane now runs in Go. The packaged engine
has no CPython runtime and invokes no Python workers. This completes replacement
of the old preview daemon/supervisor path; it does not complete every production
control-plane contract or make the Linux runtime a general Docker replacement.

Subsequent work adds [digest-bound execution profiles and native capability
negotiation](#compatibility-profiles). The measurements and test
results below describe the earlier migration checkpoint.

### Implementation

- `dockivad/api.go`: authenticated Docker API subset 1.24–1.45, image/container
  listing and inspection, create/start/stop/restart/remove, wait and framed
  snapshot logs. Unsupported options fail explicitly. Requests/body/headers and
  concurrent handlers are bounded. Tokens rotate on API restart.
- `dockivad/image.go`, `registry.go`: strict OCI JSON/digest/config validation,
  bounded layer expansion and private rootfs preparation, whiteouts, collision
  checks, atomic cache import and scoped registry pulls. Links, devices and path
  escapes are rejected. Registry requests refuse redirects/proxies; credentials
  stay on the registry origin, token requests omit them, and pulls have a total
  20-second network deadline. HTTP is test-only literal loopback opt-in.
- `dockivad/lifecycle.go`: one detached Go supervisor per running container,
  generation/status tracking, private logs and authenticated stop control.
  An inherited lock owns each run. Four concurrent runtimes and 1024 MiB reserved
  guest allocations are admitted per state store. Short-lived programs may exit
  while Stop/Restart runs; terminal status and lock cleanup resolve that race.
- `native/worker_launcher.c`: C guard applies the existing host rlimits before
  starting the native runtime. A pipe tracks supervisor liveness; supervisor death
  kills and reaps the native child. The supervisor also kills remaining group
  members when a guard exits. This is lifecycle ownership, not a sandbox claim.
- `dockivad/main.go`, `common.go`: private state, exclusive daemon ownership,
  safe file reads and atomic state records, Unix socket startup and recovery.
  Container creation publishes a complete directory with rename. Startup removes
  interrupted unpublished creation directories. Power-loss durability of arbitrary
  guest data and shared image-store reclamation remain separate storage gates.

Go validates and prepares images; C remains responsible for guest instructions,
Linux ABI/state and host adapters. The shipped shell/HTTP demos retain the
interpreter. The digest-pinned prepared Redis preset retains Unicorn. General
capability profiles and stock-image compatibility remain open.

The OCI validation working set is bounded: 32 layers, 32 MiB per blob, 128 MiB
packed image, 64 MiB expanded layers and 4096 members/paths. Temporary validation
allocations are garbage-collected and offered back to the OS after startup/image
operations. This is not an RSS ceiling. Per-store guest reservations also do not
account for the full process tree or enforce a machine-wide memory cap.

### State and upgrade behavior

The existing image-cache and container JSON formats are preserved. A running old
Python supervisor remains controllable from Go; its next start uses Go/C. The old
control client can inspect and stop a new Go supervisor for rollback. No user
rootfs rewrite or migration deletion is required.

Dockiva reuses an already running API. Opening a new app does not silently replace
an existing Python daemon. The separate Go test launcher uses
`~/.vmfree-dockiva-go-test`, so its tests exercise the new helper. Existing installed
apps and user containers were not replaced by this build.

On daemon exit/crash, supervisors and workloads continue. Restart rotates the API
token and discovers the same worker generations. On supervisor death, the guard
ends the workload; inspection marks the unowned run failed. A stale live PID is
never killed solely because it appears in an old status file. A legacy orphan
may require manual recovery before a new worker can start.

### Build and integration

```sh
python3 scripts/bundle_dockiva_engine.py --output build/dockiva-go
go -C dockivad test -race -ldflags=-linkmode=external ./...
python3 scripts/verify_dockivad.py \
  --helper build/dockiva-go/vmfree-engine/vmfree-engine \
  --report build/dockivad-recovery.json
python3 scripts/verify_dockiva_capacity.py \
  --helper build/dockiva-go/vmfree-engine/vmfree-engine \
  --report build/dockivad-capacity.json --sample-seconds 10
```

Python is development tooling only. Go uses the standard library with no external
modules. The local Go 1.22/macOS toolchain needs external linking and explicit
ad-hoc signing. Each executable is verified before bundle publication. `dockivad`
and the legacy helper filename `vmfree-engine` contain the same Go program and
are separately signed copies.

Dockiva packaging validates the five Go/C executable entries, rejects Python
files, and replaces its generated helper directory so obsolete CPython files
cannot survive a rebuild. The app metadata resolves its actual executable name.
The final app is `bin/dockiva-go-test.app` in the Dockiva checkout; double-click
`bin/Launch Dockiva Go Test.command` to use isolated engine state.

This app still uses the current Vue/Wails UI. The SwiftUI/AppKit migration and
Developer-ID/notarized clean-Mac distribution gate remain open. GUI button clicks,
older macOS releases and actual 8 GB hardware have not been qualified here.

### Validation of the final artifact

All checks below passed on Apple Silicon macOS 26.5.1, with 32 GiB host RAM:

| Check | Evidence |
| --- | --- |
| Eight Go test groups with race detector | [Go tests](../evidence/dockivad-go/go-tests.txt); OCI/JSON/path limits, fixtures, registry, API, stale state and interrupted creation |
| 21 migration/recovery checks | [Report](../evidence/dockivad-go/recovery.json), [output](../evidence/dockivad-go/recovery.txt); includes 20 immediate restarts then removal, concurrent admission, daemon/supervisor/guard crashes, token rotation, rollback control and Python-free process tree |
| 20 capacity/reconnect checks | [Report](../evidence/dockivad-go/capacity.json); 100 HTTP requests across four servers, fifth-run rejection, slot reuse and complete cleanup |
| All Dockiva Go packages with final embedded helper | [Output](../evidence/dockivad-go/dockiva-tests.txt); automatic startup/reuse, shell/HTTP and prepared Redis PING/SET/GET/AOF recovery |
| Separate live API adapter | [Output](../evidence/dockivad-go/live-adapter.txt); imported fixture lifecycle |
| Frontend/app build | [Output](../evidence/dockivad-go/app-build.txt); production frontend and Go desktop build |
| Packaging, signature, source/artifact identity | [Checks](../evidence/dockivad-go/packaging-checks.txt), [manifest](../evidence/dockivad-go/artifact.json); final embedded helper hash matches the recovery/capacity reports |

`go vet ./...` also passed in the daemon module. The test suites exercise both
old and new clients; Python appears only in the development harness and explicit
legacy-migration portion. After migration, four live containers plus their daemon,
supervisors and C guards/runtimes form a 13-process tree with no Python process.

Ten-second resource windows recorded 152.2 MiB summed RSS for the idle API
and 217.5 MiB for the API plus four HTTP workloads; CPU was
0.00% and 0.84% of one core respectively. GUI/WebViews are excluded.
These RSS values are higher than the historical Python-bundle measurements and
establish no memory improvement. Returning temporary allocations to the OS did
not reduce this RSS measure. Shared/reusable pages and physical footprint need
separate accounting; this remains an unresolved performance task, not an 8 GB
qualification or Docker comparison.

### Scope still open

Port publishing, bridge/DNS networking, general volumes/builds/Compose, streaming
logs/attach, rich versioned compatibility profiles, machine-wide resource pressure
and JIT accounting are not completed by this migration. Neither production
confinement nor Valkey V11/PostgreSQL P12 is accepted. Prepared Redis tests protect
the existing preview behavior only. No Docker Desktop RAM/CPU/battery advantage
is established.

<a id="compatibility-profiles"></a>

## Digest-bound execution profiles — 2026-09-22

Dockivad now selects an execution profile from the verified OCI manifest digest.
The Redis image tag no longer selects a backend in Go. A renamed copy of the
prepared Redis image receives the same Unicorn policy; unrelated content using
that tag receives the default interpreter policy.

This implements profile selection and launch negotiation within the existing
preview scope. Every result remains EXPERIMENTAL. It is not a general static
compatibility analyzer, stock-image qualification or a completed production gate.

### Contract

The trusted deployment catalog is [profiles.json](../dockivad/profiles.json),
embedded into the Go executable. It contains schema version 1, a default bounded
interpreter profile, and profiles bound to specific prepared image digests. OCI
labels, tags and guest files cannot supply or replace a profile.

Each definition includes ID/revision, image digests, backend, required capability
names, dynamic-loader selection, writable-root requirement, guest-memory limit,
instruction budget and evidence scope. Unknown schema fields/versions, invalid
limits and ambiguous digest mappings fail validation. The bundle builder also
rejects stale catalog pins when a prepared fixture changes.

At Create, dockivad resolves the digest, selects policy and queries the selected C
executable's `capabilities` command. The response has schema version 1, runtime ABI
`dockiva-native-v1`, and the capabilities compiled into each available backend.
Interpreter-only builds do not advertise Unicorn. Probe execution has a two-second
deadline and separately bounded 16 KiB stdout/stderr. A missing executable,
backend, ABI or required capability prevents guest launch; no fallback occurs.

Capability names describe bounded implementation features: `elf64-arm64`,
`private-rootfs`, `loopback-tcp`, `bounded-dynamic-elf` and, on Unicorn,
`cooperative-threads`. They do not assert complete ISA, loader, thread, networking
or syscall compatibility. Dynamic behavior still needs conformance/workload tests.

The effective definition, its SHA-256 identity, image digest and runtime ABI are
saved in container configuration. Start validates the saved policy against the
configuration and queries the current executable again. The supervisor repeats
validation before launching its worker. A catalog update affects new containers;
it does not silently change an existing container's backend or instruction budget.
Explicit recreation is required to select a new profile revision.

Containers made before profiles remain on their saved legacy configuration.
Inspection labels them `legacy-state` and EXPERIMENTAL, with no negotiated ABI.
Their configuration and backend are not silently migrated. Old supervisors remain
controllable through the existing state/control compatibility path.

### Inspection

The existing container Inspect response adds `DockivaCompatibility` with status,
profile ID/revision, image and definition digests, ABI, backend, requirements,
policy and evidence scope. Dockiva's existing Inspect transport preserves it.
For example, the prepared image reports:

```json
{
  "Status": "EXPERIMENTAL",
  "ProfileID": "prepared-redis-7.4.2-unicorn",
  "Revision": 1,
  "RuntimeABI": "dockiva-native-v1",
  "Backend": "unicorn"
}
```

The endpoint remains authenticated. No new image-declared privilege or arbitrary
backend override was added. The default profile's reason explicitly states that
an unqualified image passing ELF validation is not proven application-compatible.

### Verification

- Twelve race-enabled Go test groups pass, including schema rejection, real C
  capability negotiation, digest aliases, saved-policy drift, legacy state and
  rejection before container publication or guest launch.
- Ten packaged profile checks pass: both native probes, alias writable-root policy,
  inspect metadata, replaced-backend rejection, no guest on failed negotiation,
  restored Unicorn execution of renamed prepared Redis, profile persistence across
  API restart, policy-drift rejection and cleanup.
- All 21 daemon recovery checks still pass, including immediate restarts, crash
  recovery, capacity admission, old state/control migration and no Python workers.
- All Dockiva Go packages pass against the final embedded helper. Bundled tests
  inspect interpreter/Unicorn profiles and exercise shell, HTTP and prepared Redis
  PING/SET/GET and AOF persistence across Stop/Start and Restart.
- Go vet, frontend/desktop build, complete app signature and launcher syntax pass.
  GUI clicks, older macOS versions and clean-Mac distribution were not tested.

Evidence: [Go tests](../evidence/dockivad-profiles/go-tests.txt),
[packaged profile checks](../evidence/dockivad-profiles/profiles.json),
[recovery checks](../evidence/dockivad-profiles/recovery.json),
[Dockiva tests](../evidence/dockivad-profiles/dockiva-tests.txt),
[source/artifact identities](../evidence/dockivad-profiles/artifact.json).

```sh
python3 scripts/bundle_dockiva_engine.py --output build/dockiva-profiles
go -C dockivad test -race -ldflags=-linkmode=external ./...
python3 scripts/verify_dockivad_profiles.py \
  --helper build/dockiva-profiles/vmfree-engine/vmfree-engine \
  --report build/dockivad-profiles.json
```

Build the native executables before running Go tests; the suite includes real
capability probes. Python remains development tooling, absent from the shipped
process tree. The separate Dockiva app is `bin/dockiva-profiles-test.app`; its
`Launch Dockiva Profiles Test.command` uses `~/.vmfree-dockiva-profiles-test` to
avoid reusing an older daemon. Existing user stores and installed apps are intact.

### Remaining control-plane work

This does not implement dependency/ELF feature analysis, per-image production
support certification, optional-feature negotiation or profile management UI.
Networking, global resource/pressure accounting and production isolation remain
open. Valkey V11/PostgreSQL P12 and RAM/CPU/battery advantages remain unqualified.
