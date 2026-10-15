# PostgreSQL, Redis and Valkey compatibility probes

Updated 2026-09-21. The current probe also attempts real server startup in
disposable roots. None of the three servers becomes ready. This is an execution
failure, not a release-qualification rule preventing early testing.

| Application | Current native startup result | Query results |
| --- | --- | --- |
| PostgreSQL 16.13, musl | Version exits 0. With guest UID/GID 70, `initdb` creates directories/configuration, then fails bootstrap: `/dev/null` is absent and repeated child launches reach the bounded process limit. | No initialized cluster or SQL listener |
| Redis 7.4.2, glibc | Version and startup stop at unsupported scalar FDIV instruction `0x1e601821`. | No PING/SET/GET |
| Valkey 9.0.0, glibc | Version and startup stop at unsupported scalar FDIV instruction `0x1e7f1bde`. | No PING/SET/GET |

Normal and sanitizer builds reproduce these outcomes. See [startup evidence](../evidence/database-services.json)
and [sanitizer evidence](../evidence/database-services-sanitize.json).
The latest native changes are **not** a usable Dockiva database build.

This iteration adds real umask/chmod creation modes, one mapped guest identity,
additional integer/SIMD instructions, and scheduled `CLONE_VM|CLONE_VFORK|SIGCHLD`.
The parent waits until child exec/exit; shared mappings have reference-counted
ownership; successful exec detaches and failed exec preserves the old image.
The [vfork checks](../evidence/database-vfork-checks.txt) cover these behaviors,
shared mapping metadata, cleanup, and comparison with ARM64 Linux. General
shared-memory threads, futexes and pthread behavior remain unsupported.

The original failure baseline remains preserved: PostgreSQL failed guest library
allocation, while the Redis/Valkey executables exceeded the default 16 MiB input
limit. Changes now include opt-in bounded input/mapping sizes, actual-size input
allocation, loader instructions, descriptor metadata, startup auxiliary-vector
randomness and explicit errors for unsupported optional kernel facilities.
These are native interpreter changes, not replacement database builds or a VM
fallback. All original executable/library bytes remain unchanged.

## Method and limits

The script creates never-started, uniquely named fixture containers from exact
cached image IDs and exports them into temporary archives. The database executable, interpreter and transitive ELF dependencies are materialized.
The saved PostgreSQL fixture additionally includes initdb, BusyBox sh, passwd/group,
PostgreSQL catalogs/configuration templates and plpgsql. Optional LLVM JIT libraries
and the full timezone tree are not included. Links
are resolved inside the archive and copied as regular files; executable and
library bytes remain unchanged. The current prepared fixture manifests record copied files and unresolved dependencies.
An earlier initdb fixture omitted `/usr/local/lib` during dependency resolution;
that preparation bug is fixed.
The full image filesystem, environment and entrypoint are not reproduced, so this
is a direct ELF compatibility probe rather than stock-image acceptance.

Default mode runs with two million instructions. Development probes use
`--dynamic --job-control --scheduled-network --elf-limit-mib 64 --mmap-limit-mib 128`,
a 200-million-instruction cap and a 30-second host timeout. The larger
instruction budget lets PostgreSQL complete shutdown after version output. The
service replay uses a 64 MiB mmap arena for PostgreSQL so parent/exec-child images
fit within the unchanged 256 MiB aggregate allocation cap; Redis/Valkey use 128 MiB.
Separate network-disabled, read-only Linux reference containers successfully
print each version. Docker is used for fixture acquisition and reference only;
the VM-free probes execute through `build/dockiva-runtime` on macOS.
Fixture/reference containers and their owned anonymous volumes are removed.
Existing user database containers are untouched.

Database startup was attempted under VM-free, but no service became ready. There are no valid service RAM,
CPU, throughput, SQL/SET/GET, persistence or recovery results. No Docker fallback
was added to Dockiva. Default limits and production acceptance are unchanged; expanded bounds require
explicit native CLI options and still obey the aggregate memory cap.

Reproduce with the pinned cached image IDs:

```sh
python3 scripts/probe_databases.py --expanded-limits --max-steps 200000000 --startup \
  --report evidence/database-startup.json
```

Replay already prepared, hash-checked fixtures without invoking Docker:

```sh
python3 scripts/verify_database_services.py --report evidence/database-services.json
python3 scripts/verify_database_services.py --native build/dockiva-runtime-sanitize \
  --report evidence/database-services-sanitize.json
```

The replay exits nonzero when startup fails. It is a development probe: PostgreSQL
protocol checks and persistence/restart tests still need implementation once
bootstrap succeeds. The Redis/Valkey smoke test does not qualify persistence.

See [current results and hashes](../evidence/database-services.json),
[sanitizer results](../evidence/database-services-sanitize.json),
[original failure baseline](../evidence/database-probes.json), and candidate entries in
[the acceptance corpus](ACCEPTANCE-CORPUS.json). The Redis image has no recorded
repository digest; its local content image ID and binary hashes identify this
fixture, but public retrieval by repository digest remains unresolved.

## Remaining work toward M58

Implement and independently test floating-point arithmetic, virtual devices and
process lifecycle capacity, then
continue version and actual service initialization probes. M34–M38 shared memory,
thread lifecycle, futexes and TLS remain prerequisites for real application
support. M44–M48 also require locking, shared mappings, queries, Stop/Restart,
crash recovery, backup and persistence evidence. M49–M53 require ordinary Docker
and Compose workflows. M54–M58 require the backend decision and measured whole
product RAM/CPU/latency/energy qualification on representative workloads and an
8 GB machine. No milestone is accepted solely from a version command.

## Internal database test target

The immediate target is a clearly labeled internal build with Redis/Valkey
PING/SET/GET and PostgreSQL initialization/SELECT 1. It can precede production
qualification. The current Dockiva test app includes the earlier runtime build;
this latest native startup work has not been repackaged because it does not yet
produce a usable database service. The app's shell and HTTP tests do not establish
database compatibility.

The server probe uses fresh temporary storage, localhost ports and bounded native
execution. Redis/Valkey persistence is disabled for the first query smoke test.
Queries only begin after the process logs readiness. Probe-created processes,
roots and fixture containers are cleaned up; existing databases are not used.
The two harness unit tests use a fake RESP server to check detection and query
handling; they are not database acceptance evidence.
