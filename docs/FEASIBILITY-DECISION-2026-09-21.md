# VM-free feasibility decision — 2026-09-21

**Decision: redesign the execution strategy; do not resume the M34–M80 implementation sequence yet.** Keep the current interpreter as a reference and investigate a reusable CPU backend through one bounded adapter experiment. This is a conditional research recommendation, not a finding that the product can meet its performance or compatibility targets.

Scope authorized: gap inventory and an execution-feasibility assessment, capped at 10,000 tokens or 30 minutes. Runtime behavior and Dockiva were not changed. New work consists of experiment scripts, isolated build artifacts and this report. Gate A is partial: no syscall traces or exhaustive acceptance inventory. Gate B is partial: two CPU kernels measured, no integrated syscall/concurrency/backend qualification.

## What the experiments establish

### Linux database reference

The exact previously pinned images for PostgreSQL 16.13, Redis 7.4.2 and Valkey 9.0.0 all passed:

- Start the stock image, then execute a write and read it back.
- Gracefully stop/start the same container and read the value.
- Kill the server with SIGKILL, restart and read the acknowledged value.

Redis/Valkey used AOF with `appendfsync always`; PostgreSQL used its image defaults. These are single-value smoke checks, not durability, load or recovery qualification. Containers had no external network, one CPU, a 384 MiB memory limit, a 128-process limit and disposable data volumes. All recorded container/volume cleanup commands succeeded.

The local Docker endpoint runs **OrbStack Linux ARM64**, not Docker Desktop. These results establish that the chosen images work under Linux, not a Docker Desktop performance baseline. Client probes ran inside each container. PostgreSQL readiness uses TCP to avoid confusing its temporary initialization server with the final service.

Observed snapshots show six threads in each Redis/Valkey server and six PostgreSQL processes before sustained client load. Process RSS snapshots in the raw record exclude other processes and the VM; they are not whole-product RAM comparisons. `strace` was absent in all three images, so no complete syscall inventory is claimed.

Evidence: [Linux service results](../evidence/feasibility/linux-services.json), [reproduction script](../scripts/feasibility_databases.py).

### Execution backend experiment

Same assembly loop, one million iterations, matching 64-bit results, one discarded warmup and five measured runs in alternating order. Native code ran directly on this Mac; the unchanged ELF ran in our interpreter and in Unicorn 2.1.4.

| Synthetic workload | Native macOS | Current interpreter | Unicorn |
| --- | ---: | ---: | ---: |
| Integer arithmetic/branches | 2.001 ms | 94.191 ms | 3.831 ms |
| Loads/stores in a 64 KiB buffer | 0.354 ms | 152.025 ms | 21.585 ms |

In these tests Unicorn was approximately **25x and 7x faster than our interpreter**, respectively. It was still approximately **1.9x and 61x slower than native**. The current interpreter was approximately 47x and 430x slower than native. These are measured kernel-time ratios, not application speed predictions.

Important asymmetry: interpreter times include process launch/ELF loading; native timings exclude library/buffer setup; Unicorn timings exclude engine/mapping setup but include translation and two Python syscall callbacks. This favors Unicorn in the comparison. No syscall-heavy, threading, database or production-confinement workload was measured. Native numbers are not Linux-in-VM numbers. This machine has **32 GiB RAM**, so no 8 GB qualification or battery claim follows.

The initial Unicorn experiment under the tool sandbox exited with signal-like status 132. The explicitly escalated run succeeded. The cause was not isolated, and a signed/hardened application experiment remains mandatory before adoption. Apple documents specific JIT memory and entitlement requirements. [Apple JIT documentation](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon)

Evidence: [all samples, hashes and limitations](../evidence/feasibility/backend.json), [reproduction script](../scripts/feasibility_backend.py). Install is isolated under `build/feasibility/venv`; it has not been added to the runtime or app.

## Observed gap matrix

This is the known dependency inventory, not an exhaustive inventory of every possible application path.

| Area | Evidence and classification | Consequence / dependency |
| --- | --- | --- |
| CPU coverage | Confirmed missing behavior: both cache servers currently fail on scalar FDIV; numerous prior instruction additions were necessary. | A complete required instruction subset, floating-point state and exception behavior must precede service claims. Reusing a CPU engine may reduce this work. |
| Guest threads and synchronization | Confirmed architectural gap: `process_clone` accepts fork/vfork, not general shared-VM threads; no futex implementation. Reference servers each had six threads. Pinned source uses pthread creation/condition waits. | Address-space, thread, FD, TLS, signal and exit ownership must be designed together; vfork support does not supply pthreads. |
| PostgreSQL child lifecycle | Confirmed policy incompatibilities: four live child slots, sixteen lifetime children, and cloning rejected with any open socket. Reference server already has six processes. | Reusable process slots and inherited listener descriptors are prerequisites; raising one number will not fix the lifecycle model. |
| Shared memory / IPC | Confirmed missing behavior: file mappings are private only. PostgreSQL documents shared-memory and semaphore requirements. | Implement the mechanisms selected by the pinned PostgreSQL build/configuration and test interprocess visibility/lifetime. |
| Event loop | Confirmed missing epoll syscalls; pinned Redis Linux event-loop source uses epoll. Existing bounded poll support is not equivalent. | Correct readiness, close/reuse, timeout and cancellation semantics are needed for service operation. |
| Devices / full filesystem | Confirmed diagnostic failure: PostgreSQL initialization cannot open `/dev/null`. Prepared roots omit timezone data; links were materialized into regular files. | Virtual devices are runtime work; missing timezone files are fixture work. Prepared ELF success would still leave stock-image symlink/path/entrypoint acceptance open. |
| Identity / image entrypoint | Mapped UID/GID exists, but changing identities is rejected and ownership is simplified. | Stock entrypoints that initialize permissions and drop privileges need supported semantics or an explicitly supported configuration. |
| Persistence and locking | fsync/chmod exist; file-lock commands, shared writable mappings and crash ordering remain incomplete/unqualified. | Query success cannot substitute for WAL/AOF/RDB recovery and durability-policy tests. |
| Resource efficiency | Bounded arenas and copied fork images exist; performance evidence is limited to prototype workloads and these two kernels. | Measure real working sets, syscall costs and concurrency before claiming suitability for 8 GB computers. |
| Isolation and delivery | No new backend confinement, code-signing or clean-machine test in this batch. | A faster CPU engine does not automatically isolate untrusted images or ship successfully inside Dockiva. |

Local sources: `native/process.h`, `native/scheduler.h`, `native/mapping.h`, `native/runtime.c`, `native/fs.h`; [prior real VM-free failures](../evidence/database-services.json). Pinned upstream source excerpts/hashes are retained in [upstream.json](../evidence/feasibility/upstream.json). Redis [background jobs](https://github.com/redis/redis/blob/7.4.2/src/bio.c) and [epoll](https://github.com/redis/redis/blob/7.4.2/src/ae_epoll.c), Valkey [background jobs](https://github.com/valkey-io/valkey/blob/9.0.0/src/bio.c), and [PostgreSQL 16 IPC requirements](https://www.postgresql.org/docs/16/kernel-resources.html) support the relevant dependencies.

## Reuse options

| Candidate | What it offers | Assessment |
| --- | --- | --- |
| Unicorn 2.1.4 | JIT CPU emulation; macOS ARM64 wheel; successfully executed our two ELF kernels. | Best-tested candidate in this batch for reducing instruction implementation work. It supplies neither our Linux syscall layer nor validated service threading/confinement. Memory-heavy execution remains concerning. Upstream core is GPLv2; packaging metadata for Python bindings must not be mistaken for the core license. Product licensing compatibility needs review before distribution. |
| QBDI | AArch64 execution on macOS; Apache-2.0 core with bundled dependency notices. | Alternative research candidate, untested here. Its documentation explicitly excludes built-in signal and thread-creation handling. Instrumentation is not proof of a guest-memory or syscall security boundary. |
| QEMU user-mode | Existing Linux/BSD user-space ABI emulation. | Upstream documentation does not provide a drop-in Linux-on-Darwin target. Reusing/porting code would still be platform work; CPU-emulation availability on macOS does not establish Linux-user support there. |
| gVisor | Existing Linux application-kernel design. | Useful architectural reference. Its documented interception platforms depend on Linux facilities; it is not a ready macOS backend. No port was attempted. |
| Continue hand-writing the current interpreter | Familiar code and differential fixtures. | Retain as a reference, but current measurements do not justify treating it as the production execution strategy. |

Sources: [Unicorn](https://github.com/unicorn-engine/unicorn), [Unicorn 2.1.4 wheel](https://pypi.org/project/unicorn/2.1.4/), [QBDI support/limitations](https://github.com/QBDI/QBDI), [QBDI license](https://raw.githubusercontent.com/QBDI/QBDI/master/LICENSE.txt), [QEMU user-mode targets](https://www.qemu.org/docs/master/user/main.html), [gVisor platforms](https://gvisor.dev/docs/architecture_guide/platforms/). Licensing entries report upstream labels, not a determination that a particular distribution is compliant.

## Recommended next decision

Follow-up completed: [CPU adapter experiment and decision](UNICORN.md#unicorn-adapter-decision). The recommendation below records the scope proposed before that experiment.

Do not commission the whole implementation roadmap. If another batch is approved, use one bounded experiment to test an **isolated C CPU adapter**, with:

1. Unicorn syscall hooks and bounded guest memory, without wiring it into Dockiva.
2. Syscall-heavy and two-context shared-memory workloads, cancellation and invalid-access tests.
3. Separate instruction, memory-access and host-callback costs; comparison with the interpreter.
4. A signed/hardened macOS JIT launch check and a licensing compatibility decision before product integration.

Success would justify estimating a coherent Linux compatibility layer; it would not qualify a database. Stop that experiment if safe interception, memory isolation or deployment cannot be demonstrated within its agreed cap. Do not respond by resuming individual database-instruction patches.

A full stock-image syscall trace and application acceptance matrix remain outstanding. Database throughput, total RAM, CPU and energy measurements must wait for a working service backend. No honest completion date for M58/M80 can be derived from this batch.
