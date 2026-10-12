# Production Docker-replacement roadmap — M22 to M80

Architecture update 2026-09-22: [frozen baseline v1.0](FINAL-ARCHITECTURE-V1.md)
governs the production target. SwiftUI/AppKit replaces the transitional Vue/Wails
UI; Go dockivad replaces Python orchestration; the runtime and Linux Personality
remain C. Python is allowed for tooling, not production execution. ARM64 is the
first guest target; AMD64 DBT and recovery automation are deferred. Existing
milestone numbers and independent release gates remain in force.

The immediate semantic sequence is process/memory/COW, FD/VFS, IPC/futex,
events, networking and resource-pressure integration, with application probes
throughout. [Valkey V0–V11 and PostgreSQL P0–P12](WORKLOAD-GATES-V1.md) are the
first and second major compatibility gates. Shared state extraction preserves
current Redis regressions. Go/native UI migration is a required production
deliverable but does not block this runtime work. Implementation details live
in [component contracts](TARGET-ARCHITECTURE-CONTRACTS.md).

Planning baseline: 2026-09-21. M21 is the implemented prototype baseline.
M22 cancellation is implemented and locally tested; its full acceptance remains
open. See the [M1–M80 execution status](MILESTONE-STATUS.md) and
[corpus registry](ACCEPTANCE-CORPUS.json) for current implementation evidence.
M23–M26 have scoped local implementations; M27–M32 have implementation work and open acceptance gates. M33 onward remain open. M80 is a proposed
production release gate, not a delivery date or a guarantee of feasibility.
This document replaces the earlier conversational placeholders for M34 onward;
M22–M33 retain their previously proposed themes. Existing validation evidence
is not evidence that any future gate has passed.

## Product contract

The first production target is a VM-free replacement for Docker-based development
and local CI workflows on **Apple Silicon macOS using Linux ARM64 images**.
Users should be able to pull existing supported images, build supported Dockerfiles,
run their supported Compose projects, retain data, and use the documented Docker
CLI/API surface without maintaining Docker Desktop for those projects. Production
quality here means a supported desktop product; it does not automatically qualify
this runtime to replace Linux Docker Engine on production servers.

General image support requires unmodified published binaries, not guest programs
rebuilt with special compiler options. Pin image digests, application versions,
Docker clients, API ranges, macOS versions and hardware in a published support
matrix. A passing demo never makes every version of an application supported.

Initial exclusions: Windows containers, Intel Macs, AMD64-only images, privileged
containers, arbitrary kernel modules/eBPF, GPU/device passthrough, Kubernetes and
Swarm. An exclusion that a user's project needs means that project is **not**
supported as a Docker replacement. Detect known unsupported configuration before
startup, return actionable errors, and never silently introduce a VM fallback.
Further exclusions discovered during implementation must be visible release blockers
for affected projects, not hidden compatibility assumptions.

M22 begins with a proposed acceptance corpus: a web frontend/API/database/cache
Compose project, Python service, Go service, Node.js service, Java service,
SQLite tool, and Dockerfile build workflow. Select exact artifacts and workload
sizes before implementation of each application gate. This list is a planning
assumption, not a claim that these applications currently run.

## Gate rules and dependencies

Each implementation gate must have a written scope, pinned fixtures, automated
positive and negative tests, reference comparisons where meaningful, bounded
resource behavior, and preserved evidence. Security checks, fuzzing and performance
measurement run throughout; later audit milestones expand that work rather than
postpone it. Independent review cannot be replaced by our own passing tests.

Milestone numbers identify deliverables, not a mandatory serial schedule. Networking,
storage, build integration and security work may overlap once prerequisites exist.
Start architectural spikes early: full signals, threading/memory ordering, dynamic
loading, filesystem semantics, verified confinement and an efficient execution
backend are feasibility risks. Revisit architecture or narrow the support contract
when evidence requires it; adding milestone numbers does not solve those risks.

Critical path: M22–M24 process/shell semantics; M26–M28 I/O and loading; M34–M38
threading; M39–M48 target applications; M49–M53 Docker workflows; M64–M73 complete
build/migration/desktop behavior; M74–M80 user qualification and production release.
M31/M33 security and distribution gates block public previews. M59–M63 and the
final repeat audit block production qualification. Optimization work M54–M58 must
start earlier if interpretation cannot meet the agreed workload budgets.

## M22–M33: usable runtime and controlled preview

| ID | Deliverable | Prerequisites | Acceptance gate |
|---|---|---|---|
| M22 | Signals, cancellation and target corpus | M21 | Cancel individual running/blocked guests, preserve unrelated tasks, reap signal statuses; register the pinned target corpus and compatibility gaps. |
| M23 | Process groups and terminal basics | M22 | A foreground guest accepts input, Ctrl-C and EOF; host terminal state is restored on normal exit and failure. |
| M24 | Unmodified upstream BusyBox ash | M22–M23 | A pinned upstream shell passes scripts, nested process execution, expansion, pipelines and redirects against Linux references; document every excluded construct. |
| M25 | Shell job control | M23–M24 | jobs/fg/bg, stop/resume and terminal ownership work; signals affect the intended process group. |
| M26 | Scheduled networking foundation | M22, M21 | Socket readiness, timeouts and cancellation coexist with CPU/pipe tasks; slow peers do not stall unrelated guests. |
| M27 | Filesystem and descriptor compatibility | M22 | Selected utilities exercise relative directories, descriptor sharing, links and metadata; traversal and race rejection tests preserve the declared boundary. |
| M28 | ARM64 musl dynamic loading | M27 | Unmodified pinned loader/libraries and dynamic executables pass relocation, startup, shared-library and failure tests. |
| M29 | First practical application | M26–M28 | A pinned unmodified application image passes configuration, networking, file use, reload if required, and clean/crash shutdown workloads. |
| M30 | Differential compatibility harness | M24–M29 | Compare guest behavior with ARM64 Linux, classify every discrepancy, and replay a saved malformed-input/resource corpus in CI. |
| M31 | Verified confinement and aggregate limits | Starts at M22; M27–M30 | Enforce and externally review host filesystem/network access plus aggregate memory, CPU, process, descriptor and disk limits, including across guests. |
| M32 | Representative performance feasibility | M29–M31 | Publish startup, throughput, latency, CPU and total memory comparisons; agree numerical workload budgets and identify architecture blockers. |
| M33 | Controlled developer preview | M24, M29–M32 | Signed clean-Mac install, diagnostics and rollback work; only the explicit supported workloads are advertised. Open confinement blockers prevent public untrusted-image use. |

## M34–M43: threads and application runtimes

| ID | Deliverable | Prerequisites | Acceptance gate |
|---|---|---|---|
| M34 | Shared-memory guest threads | M22, M30 | Thread creation, teardown, stacks and shared address spaces pass race/lifecycle tests without corrupting sibling processes. |
| M35 | Atomics and memory ordering | M34 | Required ARM64 atomic/barrier behavior passes reference and stress suites, including reservation invalidation and adversarial interleavings. |
| M36 | Futex and synchronization support | M34–M35 | Wait/wake, timeout and cancellation behavior supports pinned mutex/condition-variable tests without lost wakeups. |
| M37 | TLS, pthread and signal integration | M22, M34–M36 | TLS isolation, pthread lifecycle, masks, handlers and required signal-frame/return behavior pass multithreaded reference tests. |
| M38 | Threaded runtime qualification | M31, M34–M37 | High thread/process churn, nested process relationships, shutdown and resource-limit tests remain bounded; reopen confinement review for the new surface. |
| M39 | glibc loader and ABI target | M28, M38 | Selected unmodified glibc-based images pass loader, locale/NSS and runtime tests for a specifically pinned library set. |
| M40 | Python application target | M26–M28, M38; M39 where required | Pinned Python service and required native extensions pass package loading, workers, networking and graceful termination. |
| M41 | Go application target | M26, M38 | Pinned unmodified Go binary passes GC pressure, goroutines, timers, network polling and signal tests. |
| M42 | Node.js application target | M26, M38–M39 | Pinned Node/V8 service passes required executable-memory/JIT behavior, worker, native-addon, event-loop and memory-pressure tests under confinement. |
| M43 | Java application target | M26, M38–M39 | Pinned JVM service passes required executable-memory/JIT behavior, GC, threads, networking and shutdown. Failure leaves Java outside the replacement scope. |

## M44–M53: stateful applications and ordinary Docker workflows

| ID | Deliverable | Prerequisites | Acceptance gate |
|---|---|---|---|
| M44 | Durable filesystem semantics | M27, M38 | Required fsync/fdatasync, atomic rename, permissions and persistence ordering survive controlled crashes; publish macOS-specific guarantees. |
| M45 | File locking and shared mappings | M35, M44 | Required locking, mmap/msync and interprocess visibility match references; unsupported mapping modes fail explicitly. |
| M46 | SQLite, Redis and Valkey targets | M26, M38, M44–M45 | Retain SQLite transaction and Redis persistence requirements; additionally pass Valkey V0–V11 with an unchanged stock ARM64 binary, BGSAVE/COW, AOF rewrite, crash/restart consistency and no named-app semantic hacks. |
| M47 | PostgreSQL target | M26, M38–M45 | Pass P0–P12 on pinned unmodified PostgreSQL, including initialization, concurrent backends, shared memory, checkpoints, forced-crash WAL recovery, checksum/data validation, sustained stress and selected upstream regression tests. |
| M48 | Backup, restore and database upgrades | M46–M47 | Restore verified backups, interrupt upgrades safely, recover expected committed data, and document rollback boundaries. |
| M49 | Versioned Docker API and CLI lifecycle | M30, M38, M44 | Published client/API matrix passes create/run/start/stop/kill/wait/rm/inspect/list with expected errors and negotiation. |
| M50 | Attach, exec, logs, events and statistics | M23, M26, M49 | Streaming, interactive exec, resize, detach, logs, exit status and reconnect behavior work with pinned clients and SDKs. |
| M51 | Volumes, bind mounts and file watching | M27, M31, M44–M45, M49 | Named-volume persistence, bounded bind access, ownership, case/symlink policy and required watch events support real source-edit workflows. |
| M52 | Container network model | M26, M31, M49 | Service DNS, outbound access, port publishing, network separation and selected IPv4/IPv6 behavior pass multi-project tests. |
| M53 | Compose application compatibility | M40–M43 for included apps, M46–M52 | Pinned projects pass services, dependencies/health, restart, profiles, networks, volumes, secrets/config delivery and up/down/recreate workflows. |

## M54–M63: performance and a hardened runtime candidate

| ID | Deliverable | Prerequisites | Acceptance gate |
|---|---|---|---|
| M54 | Execution-backend architecture decision | Start after M32; qualify against M38 | Prototype candidate execution approaches; select one against correctness, executable-memory policy, total memory and representative workload measurements. |
| M55 | Backend correctness | M54 | Differential ISA/syscall tests, self-modifying-code handling where supported and cache invalidation match the reference backend; keep a diagnostic fallback. |
| M56 | Scheduling and I/O optimization | M26, M35–M38, M55 | Reduce measured scheduling/pipe/network overhead without violating ordering, cancellation or fairness tests. |
| M57 | Image startup and storage efficiency | M44, M51, M55 | Improve measured unpack/start/reuse costs; any sharing or copy-on-write implementation preserves isolation and crash consistency. |
| M58 | Performance qualification | M32, M40–M57 | Every supported workload meets the numerical budgets frozen before optimization, including total host CPU/memory and sustained latency. Publish failures too. |
| M59 | Threat model and privilege review | M31, M38, M49–M57 | Review interpreter/backend, daemon API, image extraction, secrets, networking and mounts; validate actual host enforcement and minimize privileges. |
| M60 | Sustained fuzzing and fault injection | M30, M59 | Run coverage-tracked parsers/ISA/syscall/daemon campaigns and failure injection; retain reproducers and close blocking defects. |
| M61 | Independent security assessment | M59–M60 | External reviewers assess the shipping design; remediate and independently retest exploitable high-impact boundary failures. |
| M62 | Sustained stability and recovery | M48, M53, M58–M61 | Proposed gate: seven-day workloads plus 10,000 lifecycle cycles within fixed leak/recovery bounds, with no unexplained crash or integrity loss. |
| M63 | Hardened runtime candidate | M58–M62 | Freeze the validated runtime/API support matrix and reproducible evidence. This is an internal runtime candidate, not yet a complete Docker replacement release. |

## M64–M73: complete the build, distribution and migration workflows

These tracks may begin after M49 rather than wait for M63. They are mandatory
for a Docker replacement scope that includes local builds and everyday project use.

| ID | Deliverable | Prerequisites | Acceptance gate |
|---|---|---|---|
| M64 | Dockerfile builds and builder architecture | M24, M28/M39, M49; start feasibility work early | Choose and implement a VM-free builder/executor integration; build pinned real Dockerfiles with RUN/COPY/ADD, multistage builds and .dockerignore. No hidden VM builder. |
| M65 | BuildKit/buildx workflow target | M64, M51–M52 | Pinned buildx workflows pass cache reuse, contexts, build networking, secret/SSH mounts and failure cleanup for the advertised subset. Secrets must not persist in output layers. |
| M66 | Public and private registry workflows | M49, M59, M64–M65 | Pull/push, authentication, credential helpers, proxies/custom CAs, digest/platform selection, interrupted transfers and cache verification pass against selected registries. |
| M67 | Disk accounting and safe reclamation | M44, M51, M57, M65–M66 | Report actual image/layer/build/volume use; recover from low disk and interrupted cleanup; prune never removes referenced content or volumes without explicit authorization. |
| M68 | Compose builds, development tools and CI | M40–M43, M50–M53, M65–M66 | Selected Compose build/watch, IDE/devcontainer, SDK/Testcontainers and local CI workflows pass end to end with pinned tool versions. |
| M69 | Docker context coexistence and migration | M48–M53, M66–M68 | Explicitly switch contexts, transfer selected images/volume backups/configuration, verify data and roll back without altering unrelated Docker projects. |
| M70 | macOS runtime lifecycle | M23, M50–M52, M62 | Sleep/wake, reboot, network changes, service restart, stale sockets, laptop disconnects and interrupted operations have tested recovery behavior. |
| M71 | Self-contained native macOS distribution | M33, M59, M70 | Clean supported Macs install/uninstall a signed/notarized SwiftUI/AppKit app, Go dockivad and supervised C runtime with no Python in production execution; native UI restart preserves workloads; permissions and state are documented. |
| M72 | Safe software and state upgrades | M48, M67, M69, M71 | Signed updates and versioned state migrations survive interrupted download/install and support tested rollback or verified backup restoration. |
| M73 | Diagnostics, support and supply-chain record | M59, M66, M71–M72 | Users can export useful redacted diagnostics; ship license notices/SBOM, dependency provenance, vulnerability intake, supported versions and operational runbooks. |

## M74–M80: qualify and release the production replacement

| ID | Deliverable | Prerequisites | Acceptance gate |
|---|---|---|---|
| M74 | Docker-replacement beta | M63–M73 | Proposed entry gate: at least ten representative projects run without Docker installed in the test environment, covering build/run/Compose/storage/recovery. Label all exclusions and collect structured failures. |
| M75 | Migration and workflow closure | M74 | Resolve all blockers for the frozen target projects; users perform onboarding, daily edits/builds, restart, upgrade and rollback without undocumented workarounds. |
| M76 | Capacity and failure qualification | M58, M62, M70, M75 | Test workload concurrency, memory/disk pressure, process termination and crash recovery at advertised limits; publish tested capacity and exact durability guarantees. |
| M77 | Supported-platform qualification | M71–M72, M76 | Every claimed macOS/hardware/client combination passes the release suite; test installers, upgrades and rollback on clean machines. |
| M78 | Final independent release audit | M61, M65–M77 | Review the actual release candidate, including changes since M61; retest remediations, credentials/secrets and supply-chain/install boundaries. |
| M79 | Production release candidate | M75–M78 | Proposed gate: 30-day representative pilot, no unresolved release-blocking security/data-loss defects, accepted performance results, rehearsed recovery and signed release evidence. |
| M80 | Production Docker-replacement v1 | M79 and every mandatory product gate | Publish the supported-workload matrix, reproducible evidence, limitations, support/security policy and recovery instructions; release only after named maintainers sign off. |

## Release decision, numerical gates and stop conditions

The numerical gates above are proposed project policy, not industry certifications
or proof of universal compatibility. Freeze workload-specific thresholds at M32
and workload sizes at M22/M29, before seeing final performance results. Record
throughput, p50/p95/p99 latency where meaningful, startup, build time, total CPU,
resident/peak memory, disk growth and energy where measurement is reliable.
A reasonable initial performance *proposal* is no more than 20% degradation
against the selected Docker baseline on mandatory primary workload metrics; accept
or revise it at M32 with a documented product rationale, not after a failed M58.

M80 requires all of the following simultaneously:

- Exact supported images/projects work without custom rebuilds or undisclosed
  substitutions, including their required build and Compose workflows.
- Data recovery, isolation and resource-limit claims have direct evidence and
  independent review. A clean test run alone does not establish a secure sandbox.
- Published performance/capacity budgets pass on every mandatory workload/platform.
- Clean installation, updates, migration and rollback work with signed artifacts.
- Required beta/pilot windows pass, with no unresolved security-boundary, data-loss,
  lifecycle-hang or compatibility blocker for the advertised scope.
- A named release owner, security contact and support process are in place. Human
  rollout/signing/support decisions and external assessments are explicit work;
  they cannot be claimed complete through local implementation alone.

If a required capability cannot be implemented faithfully or securely on the chosen
architecture, pause that release claim, revise the architecture, or explicitly
exclude affected workflows. Never quietly change correctness/performance criteria
or mark an application supported because a reduced demonstration passes.

M80 is a production replacement **for the published ARM64 Mac workflow scope**.
It is not “every Docker feature on every platform.” An application's production
certification does not establish safety for every arbitrary untrusted image.

## Separate expansion programs after the initial release

These remain proposals, not prerequisites for the initial scope. If customers need
one before migration, it becomes a prerequisite for those customers' replacement
release rather than a feature to promise later.

| ID | Expansion program | Required decision/evidence |
|---|---|---|
| M81 | AMD64 Linux image compatibility | Separate ISA/ABI/backend design, correctness and performance program; do not treat cross-architecture execution as a small patch. |
| M82 | Additional host platforms | Independent host-adapter, isolation, installer and compatibility qualification for each platform. |
| M83 | Kubernetes/CRI integration | Define supported orchestration/network/storage contracts and qualify real cluster workloads. |
| M84 | Devices and GPU workloads | Determine feasibility and host security model first; validate selected runtimes and device interfaces. |
| M85 | Linux-server production deployment | Define a distinct server threat, operations and availability model, then independently qualify it. |
| M86 | Broader Docker compatibility | Expand API/plugin/orchestration/application support through separately scoped conformance gates. |

## Reference contracts

Docker client compatibility must target explicit negotiated API versions rather
than an unspecified “Docker-compatible” label. Reference:
[Docker Engine API](https://docs.docker.com/reference/api/engine/).
Compose project acceptance should map supported and rejected configuration to the
[Compose specification reference](https://docs.docker.com/reference/compose-file/).
Build acceptance has its own implementation and integration work; see
[BuildKit](https://docs.docker.com/build/buildkit/).
Persistence and migration acceptance should use the documented lifecycle of
[Docker volumes](https://docs.docker.com/engine/storage/volumes/).
These references define comparison surfaces; they do not certify this runtime.
