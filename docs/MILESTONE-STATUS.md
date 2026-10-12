# M1–M80 execution status

Updated 2026-09-22. This ledger records evidence, not a promise that a numbered
milestone is accepted. The user has requested implementation through M80.
M1–M21 contain scoped prototype implementations; their remaining qualification
limits still apply. M22 cancellation and M23 terminal basics are implemented and locally tested.
M24 bounded upstream shell scripts, M25 scoped job control, and M26 scheduled networking are implemented and locally tested. Scoped M27–M32 work is present with open acceptance gaps. M33–M80 remain unaccepted; scoped instruction, glibc-startup and filesystem work for M34/M35/M39/M44 is now present. M27–M32 are not fully accepted merely because scoped checks pass. There is no production release approval.

Acceptance criteria and dependencies remain in [the production roadmap](PRODUCTION-ROADMAP.md).
The [frozen architecture v1.0](FINAL-ARCHITECTURE-V1.md) is now the authoritative
production design. The supported preview control plane now runs in Go/C without
Python; [migration evidence](DOCKIVAD-GO-MIGRATION-2026-09-22.md) covers API, OCI,
lifecycle, recovery and packaging. Native SwiftUI/AppKit and production distribution
remain unimplemented migrations. [Valkey V0–V11 and PostgreSQL
P0–P12](WORKLOAD-GATES-V1.md) are registered OPEN; previous bounded Redis and
PostgreSQL version evidence does not automatically accept those gates.
Historical scope is in [the implementation roadmap](ROADMAP.md) and
[validation evidence](../VALIDATION.md). A later regression pass does not close
an earlier independent audit, signing, hardware, or performance requirement.

| Milestone | Deliverable | Current status |
|---|---|---|
| M1 | Independent validation | Scoped prototype implemented; see historical acceptance evidence |
| M2 | Execution backend spike | Scoped prototype implemented; see historical acceptance evidence |
| M3 | Small static programs | Scoped prototype implemented; see historical acceptance evidence |
| M4 | Guest allocation | Scoped prototype implemented; see historical acceptance evidence |
| M5 | Static HTTP server | Scoped prototype implemented; see historical acceptance evidence |
| M6 | OCI image integration | Scoped prototype implemented; see historical acceptance evidence |
| M7 | Read-only filesystem | Scoped prototype implemented; see historical acceptance evidence |
| M8 | Writable state | Scoped prototype implemented; see historical acceptance evidence |
| M9 | Static libc compatibility | Scoped prototype implemented; see historical acceptance evidence |
| M10 | Container lifecycle | Scoped prototype implemented; see historical acceptance evidence |
| M11 | Practical networking subset | Scoped prototype implemented; see historical acceptance evidence |
| M12 | Image distribution subset | Scoped prototype implemented; see historical acceptance evidence |
| M13 | Isolation review | Review/limits implemented; independent confinement acceptance OPEN |
| M14 | Performance checkpoint | HTTP timing evidence available; total RAM/CPU/battery qualification OPEN |
| M15 | Docker API subset | Scoped prototype implemented; see historical acceptance evidence |
| M16 | Compose subset | Scoped prototype implemented; see historical acceptance evidence |
| M17 | Mac distribution tooling | Distribution tooling implemented; signed clean-Mac acceptance OPEN |
| M18 | Selected upstream BusyBox commands | Scoped prototype implemented; see historical acceptance evidence |
| M19 | Bounded processes and basic shell | Scoped prototype implemented; see historical acceptance evidence |
| M20 | Pipes and descriptor redirection | Scoped prototype implemented; see historical acceptance evidence |
| M21 | Concurrent streaming pipelines | Scoped prototype implemented; see historical acceptance evidence |
| M22 | Signals, cancellation and target corpus | Cancellation tested; portable Linux signal comparison passed; production corpus OPEN |
| M23 | Process groups and terminal basics | Implemented; 19 PTY/group checks and sanitizer checks passed; prerequisite corpus gate remains open |
| M24 | Unmodified upstream BusyBox ash | Implemented scoped ash scripts; 89 local checks and 26 Linux comparisons passed; traps/job control excluded |
| M25 | Shell job control | Scoped implementation: 25 local/sanitizer checks and 5 Linux comparisons passed; raw terminal modes remain unsupported |
| M26 | Scheduled networking foundation | 29 local/lifecycle/Linux checks, sanitizer cleanup probes and 30 HTTP/product regressions passed; bounded loopback scope |
| M27 | Filesystem and descriptor compatibility | In progress: cwd/fchdir, shared regular offsets and forked directory cursors tested; links, directory dup, parent traversal and fuller metadata OPEN |
| M28 | ARM64 musl dynamic loading | Scoped loader implemented: 26 checks including 5 Linux comparisons; broader mapping/ELF qualification OPEN |
| M29 | First practical application | Unmodified wget candidate implemented; HTTP/timeout/cancellation/OCI/lifecycle checks; prerequisite filesystem/confinement gates OPEN |
| M30 | Differential compatibility harness | Implemented: 8 Linux matches, 2 declared policy gaps, 5 malformed/resource replays; hosted CI execution OPEN |
| M31 | Verified confinement and aggregate limits | In progress: allocator/storage/admission limits and client-only Seatbelt tests; independent review/server confinement/cross-store limits OPEN |
| M32 | Representative performance feasibility | Measurement harness and 16 standalone samples present; representative stacks, agreed budgets, Docker Desktop and whole-product accounting OPEN |
| M33 | Controlled developer preview | Not implemented / not accepted |
| M34 | Shared-memory guest threads | Partial shared-image/vfork lifetime support; general threads and qualification OPEN |
| M35 | Atomics and memory ordering | Partial instruction support and Linux/sanitizer fixture; shared-thread ordering/stress gate OPEN |
| M36 | Futex and synchronization support | Not implemented / not accepted |
| M37 | TLS, pthread and signal integration | Not implemented / not accepted |
| M38 | Threaded runtime qualification | Not implemented / not accepted |
| M39 | glibc loader and ABI target | In progress: bounded loader, aux vector and metadata support; target application qualification OPEN |
| M40 | Python application target | Not implemented / not accepted |
| M41 | Go application target | Not implemented / not accepted |
| M42 | Node.js application target | Not implemented / not accepted |
| M43 | Java application target | Not implemented / not accepted |
| M44 | Durable filesystem semantics | Partial sync/access/readlink support; crash ordering, permissions and recovery OPEN |
| M45 | File locking and shared mappings | Not implemented / not accepted |
| M46 | SQLite, Redis and Valkey targets | Prepared Redis queries, background RDB/AOF rewrite with parent writes/restart isolation, four/eight-client contention and AOF process-crash recovery pass on opt-in Unicorn; Valkey V0–V11 and broader M46/stock-image gates remain open |
| M47 | PostgreSQL target | Direct ELF version matches Linux; initdb creates configuration but bootstrap fails on missing virtual device/process capacity; queries and persistence OPEN |
| M48 | Backup, restore and database upgrades | Not implemented / not accepted |
| M49 | Versioned Docker API and CLI lifecycle | Not implemented / not accepted |
| M50 | Attach, exec, logs, events and statistics | Not implemented / not accepted |
| M51 | Volumes, bind mounts and file watching | Not implemented / not accepted |
| M52 | Container network model | Not implemented / not accepted |
| M53 | Compose application compatibility | Not implemented / not accepted |
| M54 | Execution-backend architecture decision | Frozen baseline adopted; CPU/address-space state and revision-driven backend mapping synchronization tested; reference-counted backing, interpreter COW and Unicorn COW write faults tested; macOS host-page COW and bounded private Unicorn fork now tested; full interface, MAP_SHARED and comparative qualification remain open; not accepted |
| M55 | Backend correctness | Not implemented / not accepted |
| M56 | Scheduling and I/O optimization | Not implemented / not accepted |
| M57 | Image startup and storage efficiency | Not implemented / not accepted |
| M58 | Performance qualification | Not implemented / not accepted |
| M59 | Threat model and privilege review | Not implemented / not accepted |
| M60 | Sustained fuzzing and fault injection | Not implemented / not accepted |
| M61 | Independent security assessment | Not implemented / not accepted |
| M62 | Sustained stability and recovery | Not implemented / not accepted |
| M63 | Hardened runtime candidate | Not implemented / not accepted |
| M64 | Dockerfile builds and builder architecture | Not implemented / not accepted |
| M65 | BuildKit/buildx workflow target | Not implemented / not accepted |
| M66 | Public and private registry workflows | Not implemented / not accepted |
| M67 | Disk accounting and safe reclamation | Not implemented / not accepted |
| M68 | Compose builds, development tools and CI | Not implemented / not accepted |
| M69 | Docker context coexistence and migration | Not implemented / not accepted |
| M70 | macOS runtime lifecycle | Not implemented / not accepted |
| M71 | Self-contained native macOS distribution | SwiftUI/AppKit and Python-free Go/C production migration required; signed/notarized clean-Mac acceptance open |
| M72 | Safe software and state upgrades | Not implemented / not accepted |
| M73 | Diagnostics, support and supply-chain record | Not implemented / not accepted |
| M74 | Docker-replacement beta | Not implemented / not accepted |
| M75 | Migration and workflow closure | Not implemented / not accepted |
| M76 | Capacity and failure qualification | Not implemented / not accepted |
| M77 | Supported-platform qualification | Not implemented / not accepted |
| M78 | Final independent release audit | Not implemented / not accepted |
| M79 | Production release candidate | Not implemented / not accepted |
| M80 | Production Docker-replacement v1 | Not implemented / not accepted |

Latest CPU experiment: [prepared Redis query and AOF clean-restart acceptance](REDIS-QUERY-PERSISTENCE-2026-09-22.md) passes on opt-in Unicorn. This bounded fixture result does not complete a whole milestone. The separate [Dockiva Redis test package](DOCKIVA-REDIS-PREVIEW-2026-09-22.md) now passes bundled adapter checks; native GUI clicks and distribution remain unqualified.

## Immediate remaining work

The current architecture implementation order is in
[target contracts](TARGET-ARCHITECTURE-CONTRACTS.md#delivery-sequence-and-acceptance-gates).
Start with explicit state ownership and the common backend boundary, preserving
prepared Redis acceptance; then address database process/shared-memory behavior.
Go daemon migration and the required native UI replacement are production
deliverables, but are not prerequisites for those runtime steps.
This prioritization does not waive the earlier acceptance gaps listed below.
The [CPU-state extraction](CPU-STATE-OWNERSHIP-2026-09-22.md) and
[address-space state/backend synchronization](ADDRESS-SPACE-OWNERSHIP-2026-09-22.md)
have passed their local regression gates. [Reference-counted backing](MEMORY-BACKING-OWNERSHIP-2026-09-22.md)
also passes. [Private interpreter fork COW](PRIVATE-FORK-COW-2026-09-22.md) now passes
isolation, syscall-write, mapping and vfork checks. [macOS host-page COW and
bounded Unicorn private fork](PAGE-COW-UNICORN-FORK-2026-09-23.md) now pass process, descriptor, thread and
exec tests plus prepared Redis background persistence/restart isolation.
MAP_SHARED, complete signals/exec and PostgreSQL shared-memory workflows remain open.

1. M22: select and pin unmodified production artifacts and workload sizes in
   [the corpus registry](ACCEPTANCE-CORPUS.json). Portable signal status behavior
   matches ARM64 Linux; broader cancellation comparisons remain open. Null image
   digests mean selection is incomplete.
2. M23: guest process groups and foreground input, with Ctrl-C/EOF, terminal
   restoration, redirected-input behavior and failure-path tests.
3. M24–M30: upstream shell, scheduled networking, filesystem compatibility,
   dynamic loading, a practical application and differential testing.
4. Follow the remaining dependency gates through M80, reopening confinement and
   compatibility review as the runtime surface expands.

## Release evidence that local coding cannot replace

Independent assessments (M31/M61/M78), clean-Mac hardware qualification,
release signing, representative beta users, the sustained stability window,
the 30-day pilot and named maintainer signoff remain required. None is recorded
as passed merely because a local verifier succeeds.

RAM, CPU and battery comparisons remain required performance evidence. No
unmeasured savings are claimed. Existing OrbStack timing results are a separate
baseline from Docker Desktop; capture both names and versions in future reports.

## Dockiva internal integration — 2026-09-21

Added a separately selected Dockiva backend using the authenticated Unix API, supported-image listing and read-only creation, lifecycle, inspect and snapshot logs. The live adapter test passed against a temporary M19 fixture store. This is integration engineering, not M33 preview acceptance; existing M27–M32 and M80 gates remain open. See [project definition](PROJECT-DEFINITION.md).

The 2026-09-21 build bundled Python, the native runtime, API/supervisor and M19 demo; the Go checkpoint below supersedes that packaging. New Apple Silicon installations default to VM-free with automatic launch; selecting it starts it. Bundled-engine startup and guest-execution tests passed from the signed local app with no Python/Homebrew in PATH. This remains internal testing and does not close production gates.

## Active implementation through M58

The current requested endpoint is M58. Work first removes observed database
loader/ISA/ABI blockers, with each change retaining bounded failure behavior.
See [M35](MILESTONE-35.md), [M39](MILESTONE-39.md), [M44](MILESTONE-44.md) and
[database probes](DATABASE-COMPATIBILITY-PROBES.md). PostgreSQL version success
is not PostgreSQL service acceptance. A separate local Dockiva Redis test build packages the opt-in adapter and prepared Redis fixture. The default installed app remains unchanged.

Remaining critical path: M34–M38 shared memory and thread lifecycle/futex/TLS;
M39–M43 complete application runtimes; M44–M48 persistence and database services;
M49–M53 ordinary Docker/Compose workflows; M54–M58 execution architecture,
optimization and measured performance. Independent confinement/signing and real
8 GB hardware gates are still open. No savings or M58 acceptance is claimed.

## Go dockivad checkpoint — 2026-09-22

The existing preview API and supervisor path have moved to Go, with C guards and
execution workers. Eight race-enabled Go test groups, 21 recovery/migration checks
and Dockiva integration checks pass. OCI imports, digest verification, private
state, bounded logs and four-container admission retain their documented scope.
Daemon crashes preserve workloads; supervisor/guard crashes close their listeners
and permit recovery. The helper has no CPython or Python worker dependency.

This is control-plane migration evidence, not acceptance of M33–M80. General
port publishing, resource/pressure management, stock-image compatibility, the
native UI, production isolation and distribution remain open.

## Dockivad compatibility profiles — 2026-09-22

[Digest-bound execution profiles](COMPATIBILITY-PROFILES-2026-09-22.md) replace
Go tag-specific backend selection. Schema/revision, required native capabilities,
policy limits and evidence scope are persisted and exposed through Inspect.
Create/Start/supervisor launch reject mismatches before guest execution. Legacy
state remains explicitly unprofiled; deployment updates do not rewrite its policy.
Twelve Go test groups, ten packaged profile checks, the 21 recovery checks and
Dockiva's bundled service tests pass. Production compatibility/isolation gates
remain open; this is no new RAM/CPU/battery or database qualification.
