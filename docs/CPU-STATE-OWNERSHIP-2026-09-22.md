# First CPU-state ownership extraction — 2026-09-22

The first part of architecture slice A is implemented. It gives CPU state an
explicit owner and uses the same representation in interpreter process snapshots
and the Unicorn register-transfer boundary. It does not complete the full
[state/backend architecture contract](TARGET-ARCHITECTURE-CONTRACTS.md).

## Changes and ownership

[GuestCpuState](../native/cpu_state.h) now contains general registers, PC/SP, TLS,
condition flags, SIMD registers, FPCR/FPSR storage and the interpreter's exclusive
reservation metadata. A `RuntimeContext` owns the active CPU state. Existing
instruction and syscall code accesses it through a transitional active CPU pointer;
the runtime remains cooperative and is not reentrant or safe for parallel host
execution merely because these fields have moved.

| Previous storage | Owner/boundary now |
|---|---|
| `regs`, `pc`, `sp`, condition flags | `GuestCpuState` |
| `thread_pointer`, `vectors` | CPU TLS and SIMD fields |
| Interpreter exclusive-address/size/valid globals | CPU reservation fields; validity still cleared on process restore |
| Separate CPU fields in `ProcessImage` | One CPU snapshot copied by save/restore |
| Unicorn scalar-only transfer with implicit SIMD/FP residency | Explicit `uc_transfer_cpu(engine, state, direction)` including SIMD and FPCR/FPSR, using batch register APIs |

Unicorn's opaque per-thread contexts continue to preserve backend-private state,
including its own exclusive monitor. This change does not replace Unicorn's
thread scheduler. FPCR/FPSR storage does not add unsupported interpreter floating
point instructions. The old trace record's `pc` field was renamed `address`;
diagnostic output is unchanged.

Address-space, descriptor and scheduler ownership remain to be extracted. The
full common execution/yield/fault interface is still pending. No new Linux
syscall, fork, shared-memory or application support is claimed.

## Validation

The [CPU-state harness](../tests/native/cpu_state_host.c) alternates 100 executions
between two distinct CPU owners using real interpreter instructions and process
snapshots. It checks that the inactive owner is unchanged, that registers,
TLS/SIMD/FP state survive restoration, and that teardown clears state and releases
the test allocation. This runs under AddressSanitizer and UndefinedBehaviorSanitizer.
A signed Unicorn harness additionally alternates 100 register transfers, checks
special registers through independent Unicorn IDs, and checks engine-side changes
are visible on readback. Both passed.

| Regression gate | Result |
|---|---|
| Experimental backend ELF checks | 14 passed |
| Thread/futex probes | Three Unicorn checks and two ARM64 Linux reference checks passed |
| Interpreter upstream shell/process corpus | 89 checks passed |
| Interpreter dynamic-loader corpus | 21 checks passed |
| Python unit tests | 63 passed |
| Prepared Redis query and clean restart | Both cycles passed |
| Redis eight-client contention and recovery | Three clean cycles, two forced-crash cycles and final recovery passed; 651 acknowledged keys retained |
| Rebuilt bundled engine through Dockiva adapter | Four tests passed, including automatic engine startup and Redis Stop/Start/Restart persistence; unrelated external live-engine test explicitly skipped |

Two alternating before/after query-and-restart timing runs took 3.52/2.80 seconds
before and 2.85/2.92 seconds after. This small diagnostic does not establish a
performance improvement or qualify overhead, memory, CPU or battery use.

The freshly packaged helper is at `build/dockiva-state-ownership/vmfree-engine`.
The existing open Dockiva test application was not replaced. The packaged adapter
check uses the new helper in isolated temporary state; native GUI clicks were not
part of this validation.

Evidence: [aggregate report and artifact/source hashes](../evidence/state-ownership/report.json),
[CPU harness](../evidence/state-ownership/cpu-state.json),
[backend](../evidence/state-ownership/backend.json),
[threads and Linux comparisons](../evidence/state-ownership/threads.json),
[Redis recovery](../evidence/state-ownership/redis-recovery.json),
[Dockiva adapter](../evidence/state-ownership/dockiva-tests.txt),
[diagnostic timing](../evidence/state-ownership/timing/comparison.json).
Run the new harness with `python3 scripts/verify_cpu_state.py` after building the
local Unicorn dependency. It signs its macOS Unicorn test binary for JIT use.

## Next implementation

Extract the current address-space state and ownership of its backing allocations,
then connect mapping/protection changes to backend invalidation explicitly.
Preserve the current CPU, interpreter and Redis gates. This remains slice A;
general process/shared-memory implementation follows in slice B. Slice C uses the
frozen baseline's Valkey V0–V11 gate first, then PostgreSQL P0–P12, retaining the
existing Redis tests throughout.
