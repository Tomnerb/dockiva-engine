# Frozen architecture v1.0 workload gates

These gates operationalize sections 15 and 21 of the [final specification](FINAL-ARCHITECTURE-V1.md). Valkey persistence is the first major compatibility gate; PostgreSQL crash recovery and sustained operation are the second. The [machine-readable register](WORKLOAD-GATES-V1.json) starts OPEN. Existing Redis acceptance and PostgreSQL version probes remain evidence of their original bounded scope and do not automatically accept this newly pinned gate matrix.

## Evidence contract

Before accepting a workload gate, pin the image digest, unchanged binary hash, configuration, runtime/backend build, ARM64 Linux reference environment, dataset, client count, duration, timeouts and limits. Record commands, results, logs and artifact hashes. Version/configuration/backend changes trigger impact review and relevant requalification. An application name or mutable tag is not sufficient.

Crash tests distinguish guest/process termination from host power loss. Declare fsync/durability settings and the acknowledgement oracle before running; assert the promised data guarantees. A process-crash pass is not a power-loss result. No claim about an 8 GB host is accepted from a larger machine alone.

Small semantic probes and application startup probes run throughout development. The order below controls acceptance dependencies, not a requirement to postpone all workload testing until every subsystem is built. Preserve the [Redis recovery regression](REDIS-CONCURRENCY-RECOVERY-2026-09-22.md) while implementing generic semantics.

## Valkey: V0–V11

| Gate | Behavior | Acceptance | Status |
|---|---|---|---|
| V0 | Server starts | Pinned unchanged Linux ARM64 executable reaches readiness; record binary and image digests. | OPEN |
| V1 | PING | Protocol response is correct; startup/loading is handled explicitly. | OPEN |
| V2 | SET / GET | Values and missing-key behavior match the reference. | OPEN |
| V3 | Concurrent clients | Concurrent reads/writes and atomic operations retain the expected results. | OPEN |
| V4 | Sustained workload | A predefined duration, dataset and concurrency complete within declared resource limits. | OPEN |
| V5 | SAVE | Synchronous snapshot completes and restores the expected dataset. | OPEN |
| V6 | BGSAVE | Background snapshot validates fork/COW, continued parent writes, child completion, wait/signals and restored contents. | OPEN |
| V7 | AOF | Declared append/fsync policy persists acknowledged writes across the specified restart scenarios. | OPEN |
| V8 | AOF rewrite | Background rewrite with concurrent writes produces a valid recoverable dataset. | OPEN |
| V9 | Graceful shutdown | Guest shutdown completes within the deadline with expected persistence and released resources. | OPEN |
| V10 | Forced crash | Terminate during the pinned write workload; capture acknowledged operations and files for recovery. | OPEN |
| V11 | Restart + persistence validation | Recover after V10 and validate contents against the declared durability policy; no named-application semantic branches. | OPEN |

## PostgreSQL: P0–P12

| Gate | Behavior | Acceptance | Status |
|---|---|---|---|
| P0 | postgres --version | Pinned unchanged Linux ARM64 executable returns the expected version and exit status. | OPEN |
| P1 | initdb | Initialization completes with valid catalog/data files and no hidden fixture substitutions. | OPEN |
| P2 | Server startup | The initialized server reaches readiness with required background processes. | OPEN |
| P3 | Single connection | A real client connects and completes a query. | OPEN |
| P4 | CRUD | Create/read/update/delete and transaction commit/rollback produce expected results. | OPEN |
| P5 | Concurrent backends | Independent sessions and transactions run without cross-process state corruption. | OPEN |
| P6 | Shared-memory workloads | Exercise shared state and inter-process synchronization under pinned contention. | OPEN |
| P7 | WAL behavior | Validate WAL and acknowledged commits under the declared durability settings. | OPEN |
| P8 | Checkpoint | Checkpoint while active workloads run; verify subsequent restart. | OPEN |
| P9 | Forced crash during writes | Crash at recorded write/checkpoint phases and retain the acknowledgement oracle. | OPEN |
| P10 | WAL recovery | Restart from P9 without manual repair and recover according to the declared guarantees. | OPEN |
| P11 | Checksum/data validation | Check recovered contents and enabled checksum/integrity mechanisms; report lost or unexpected committed data. | OPEN |
| P12 | Long-running stress test | A pinned duration/load includes concurrent operations, resource observations and recovery; all preceding gates hold. | OPEN |

## Scope and milestone mapping

M46 retains SQLite and Redis requirements and adds Valkey V0–V11. M47 incorporates PostgreSQL P0–P12 without dropping existing image/regression requirements. M48 adds backup/restore/upgrade validation beyond these gates. Their acceptance remains open. The broader M1–M80 compatibility, isolation, distribution and pilot gates remain required.

SUPPORTED requires active evidence for the stated configuration. COMPATIBLE means required features appear available but are not qualified by the supported matrix. EXPERIMENTAL exposes incomplete or insufficiently validated behavior. UNSUPPORTED includes an explicit reason. Static analysis cannot establish every dynamic requirement; runtime failures must identify the missing capability when encountered.
