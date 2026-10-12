# VM-free runtime feasibility and delivery plan

Planning draft, 2026-09-21. No gate below has passed. This is a proposed change
in execution order, not another promise to complete M80. Implementation is not
authorized by this planning document alone.

## Product contract

First target: Dockiva on Apple Silicon macOS, running pinned, unchanged Linux
ARM64 PostgreSQL, Redis and Valkey binaries without a Linux VM or host-native
database substitution. The eventual supported workload must run acceptably on
a real 8 GB Mac. Support for three databases is narrower than Docker replacement.
Windows, AMD64, arbitrary images and broad Docker compatibility follow only
after this target is demonstrated.

The immediate deliverable is a feasibility decision, not more numbered milestones.
Existing OCI/Dockiva integration, fixtures and differential tests are reusable
assets subject to regression checks; their existence does not prove feasibility.

## Gates, in order

| Gate | Work and evidence | Condition for proceeding |
| --- | --- | --- |
| A. Inventory and acceptance contract | Capture stock-image Linux reference traces for startup, queries, persistence and shutdown. Combine these with upstream source/tests to inventory instructions, syscalls, process/thread behavior, IPC, devices and filesystem needs. Traces are observations, not exhaustive specifications. Separate fixture omissions, emulator defects and deliberate policy restrictions. | A versioned gap matrix, pinned workload definitions, dependency order and bounded estimates for the next experiment. No remaining claim that fixing the next observed failure will finish the application. |
| B. Execution feasibility | Measure the current interpreter on representative compute, allocation, syscall and concurrency workloads. Investigate reusable execution components and a bounded block-translation prototype only if needed. Verify actual macOS/ARM64 host support, license, memory isolation, syscall interception and executable-memory constraints before selecting a component. | A defensible route to adequate throughput/latency and isolation. Keep the interpreter as a differential reference if a faster backend is selected. A backend that only supports Linux hosts is not a drop-in macOS solution. If no route fits the agreed budget, stop implementation and report that outcome. |
| C. Shared runtime foundations | Implement coherent process/thread/FD/address-space ownership, reusable reaped process slots, TLS, atomics, futex waits/wakes, signals, poll/epoll behavior, required floating-point semantics, virtual devices and required filesystem operations. Preserve aggregate limits while removing incidental prototype restrictions. Define and test floating-point rounding, exceptions and NaNs; do not just special-case current instruction words. | Focused Linux differential tests, error paths, cancellation, concurrency stress and sanitizer checks pass. Unsupported behavior returns an explicit error. Tests must check semantics, not merely repeat the implementation. |
| D. Redis service | Start the pinned Redis image with the intended configuration and full image filesystem. Verify PING, SET/GET, expiration, concurrent clients, RDB and AOF recovery, graceful restart and abrupt termination. Begin with a prepared ELF root only as an intermediate diagnostic step. | Automated service tests pass, acknowledged writes survive the selected durability policy, and a repeated workload shows no unexplained resource growth. A version command or readiness log alone fails this gate. |
| E. Valkey and PostgreSQL services | Qualify Valkey independently. For PostgreSQL, finish initdb, required shared memory/semaphores/locking, backend processes and WAL behavior; then test SQL, transactions, concurrent clients and restart/crash recovery. Include a supported backup/restore round trip. | All three pinned services pass their own query and persistence suites. Explicitly document unsupported extensions, versions and configurations. Passing Redis does not qualify Valkey. |
| F. Dockiva delivery | Package the qualified runtime. Launch from Dockiva with the selected engine, expose reachable ports, durable volumes, logs, failures and Stop/Restart. Test closing/reopening Dockiva and restarting the engine without losing data. | A reproducible packaged-app test on a clean supported Mac uses the VM-free engine throughout. No development shell, system Python or silent fallback is required. |
| G. 8 GB resource qualification | Compare the same versions, configurations, data and offered workload with Docker Desktop on the same physical machine. Count Dockiva UI, engine, guest processes and helpers; separately report storage/cache effects, idle use, loaded use, latency and throughput. Measure energy per completed workload and a controlled idle interval, with repeated runs and uncertainty. | Pre-agreed numerical budgets pass on real 8 GB hardware. Memory savings accompanied by unacceptable latency or greater CPU/energy cost do not pass. Hardware measurements cannot be replaced by a memory-limited test on a larger Mac. |
| H. Broader Docker replacement | Resume the existing roadmap selectively: supported Docker CLI/API, Compose, builds, mounts, networking, upgrades, migration and compatibility matrix. Run independent confinement review and clean-install/update qualification. | Release only the workflows demonstrated by end-to-end tests and documented exclusions. Three working databases do not imply arbitrary Docker compatibility. |

## Performance decision before feature expansion

Gate B runs early; performance is not deferred until every feature exists.
Full database and whole-product measurements recur at D through G because
microbenchmarks cannot predict total application behavior.

Suggested **discussion targets**, not measurements or accepted commitments:
at least 80% of baseline throughput, at most 1.25 times baseline p95 latency,
and at least 20% lower total product memory for the agreed small development
stack. CPU and battery savings must be independently measured; do not infer
them from RAM. Replace these targets with user-agreed limits before judging a
backend. Report raw absolute values as well as ratios, including swap pressure.

## Spending and stop rules

- Begin with Gate A and a tightly scoped Gate B experiment. Do not authorize the
  remaining implementation automatically from this plan.
- Before each batch, state the question, artifact, success condition and proposed
  time/token cap. The user selects the cap; no budget is assumed here.
- Stop at the cap and deliver findings. New architectural dependencies trigger a
  revised estimate and decision, not another unbounded patch sequence.
- Every report distinguishes implemented code, passing tests and usable product
  behavior. Record failed experiments and uncertainty as plainly as successes.
- No completion dates for later gates until their prerequisite evidence exists.
- Do not spend on Windows, broad image support or release packaging while the
  first service/backend feasibility gates remain unresolved.

## Current evidence and next artifact

The current [database report](DATABASE-COMPATIBILITY-PROBES.md) shows failed
PostgreSQL bootstrap and unsupported scalar division in Redis/Valkey. General
threading, synchronization and service persistence remain unqualified.

The next artifact should be one short architecture decision report: the complete
observed gap matrix, representative baseline measurements, viable backend
candidates with platform constraints, and a recommendation to continue, redesign
or stop. No new database-support or Docker-replacement claim follows from the
current prototype alone.

## Sources informing the plan

- [PostgreSQL kernel resources](https://www.postgresql.org/docs/18/kernel-resources.html)
  documents shared-memory and semaphore requirements. Validate the exact pinned
  PostgreSQL version/build configuration when writing its acceptance suite.
- [gVisor platform guide](https://gvisor.dev/docs/architecture_guide/platforms/)
  describes Linux-dependent syscall interception platforms; reuse requires a
  host-platform analysis, not merely adopting an application kernel.
- [gVisor compatibility guide](https://gvisor.dev/docs/user_guide/compatibility/)
  illustrates application-level regression qualification alongside ABI support.
