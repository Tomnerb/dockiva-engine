# M11–M17 candidate validation

- Linux x86-64: 46 checks passed with all optional clients/references enabled.
- Default acceptance: 30 checks covering persistent/concurrent HTTP, local OCI
  registry pull/cache, policy failures, API auth, resource limits and Compose state.
- Docker CLI 27.5.1: nine real-client checks passed.
- Docker Compose 2.32.4: six checks passed, including actual frontend/backend
  traffic through two interpreter processes and stop/restart/down.
- One network ABI fixture passed Linux QEMU user-mode. No VM is used by the runtime.
- The 30 default checks passed with AddressSanitizer/UndefinedBehaviorSanitizer.
  Leak detection was disabled because this environment cannot inspect process tasks.
- Earlier M2 (20), M3 (22), M4 (17), M5 (14), M6 (24), M7–M10 (40) regressions passed.
- All 31 Python unittest methods passed.
- Three Linux runtime-only HTTP benchmark samples captured; no VM/CPU/RSS comparison.
- Local registry fixture is real HTTP with OCI endpoints. Public HTTPS/auth/CDN
  behavior has not been accepted end to end.
- M0–M10 passed on the user's Mac per report. Candidate Mac acceptance pending.
- Production isolation review, same-Mac VM/resource comparison and signed clean-Mac
  distribution gates remain open. Mac packaging scripts were not executed here.

# Historical combined Milestones 7–10 validation

- Linux x86-64: 40 combined checks passed; exact case results in
  evidence/m7-10-linux-validation.json.
- Four ordinary static musl cases and an independent ISA probe matched Linux
  QEMU user-mode on stdout, stderr and exit code. Filesystem/lifecycle policy
  checks are not independent Linux conformance comparisons.
- HTML read from the image produced the expected HTTP response.
- Independent private trees, restart persistence, logs, exit status, stop,
  lifecycle locking and unauthenticated control rejection passed.
- First 39 checks passed AddressSanitizer and UndefinedBehaviorSanitizer. The
  subsequently added control authentication test is Python-only. Leak detection
  was disabled due to environment process-inspection restrictions.
- Earlier M6 (24), M5 (14), M4 (17), M3 (22), M2 (20) suites passed.
- All 31 Python unittest methods passed.
- M0–M6 Mac acceptance passed per user report; M7–M10 Mac acceptance pending.
- No native ARM64 Linux hardware, Docker API, isolation or performance claim.

# Historical Milestone 6 validation

- Linux x86-64: all 24 OCI loader/integration checks passed; report in
  evidence/linux-m6-validation.json.
- Two HTTP requests executed from an OCI image produced exact expected responses.
- Image Cmd/Env and Cmd override produced expected guest output and exit status.
- SIGTERM forwarded, child listener closed, temporary extraction directory removed.
- Earlier 14 M5, 17 M4, 22 M3 and 20 M2 checks passed with the updated C backend.
- All 31 unittest methods passed (13 original and 18 OCI).
- M0–M5 Mac acceptance passed per user report. M6 Mac acceptance pending.
- No new Linux OCI reference, sanitizer or performance comparison claimed for M6.
- Native ARM64 Linux hardware validation and secure isolation remain open.

# Historical Milestone 5 validation

- Linux x86-64: all 14 network/HTTP checks passed.
- Nine cases matched Linux QEMU user-mode: six end-to-end HTTP/disconnect
  scenarios and three unambiguous descriptor/pointer error cases. Five checks
  concern runtime policy, not full Linux behavior.
- AddressSanitizer/UndefinedBehaviorSanitizer: all 14 checks passed. Leak detection
  was disabled because this environment cannot inspect process tasks.
- Existing 17 M4, 22 M3, 20 M2 checks and 13 Python unittest methods passed.
- Two real curl requests to successive runs on port 18080 returned the expected
  body and guest exit status 0, validating guest SO_REUSEADDR and restart behavior.
- Guest fixtures built independently with Zig 0.13.0/LLVM and constrained integer
  register freestanding C. Build flags and source/ELF hashes are included.
- M0 through M4 Mac acceptance passed per user report. M5 Mac acceptance pending.
- Native ARM64 Linux hardware execution remains unverified.
- No general Linux networking, libc, concurrent server, isolation or Docker claim.

# Historical Milestone 4 validation

- Linux x86-64: 17 allocation checks passed with the compiled C backend.
- Ten supported-behavior cases matched QEMU 7.2.22 user-mode on Linux on exact
  stdout, stderr and exit status. Three cap/feature-policy cases and four access
  faults were runtime-only checks, not Linux conformance comparisons.
- All 17 cases also passed AddressSanitizer/UndefinedBehaviorSanitizer checks.
  Leak detection was disabled because environment process inspection is unavailable.
- All 22 M3 checks, 20 M2 checks and 13 original Python unittest methods passed.
- Fixtures were built by Zig 0.13.0/LLVM with freestanding integer-only O0 C.
- Compiler metadata, native C/header hashes and fixture manifests are included.
- M0-M3 Mac acceptance passed per user report. M4 Mac acceptance is pending.
- Native ARM64 Linux hardware validation remains pending.
- No general libc allocator, networking, Docker, isolation or performance claim.
- Heap backing memory is retained for reuse until process exit; guest unmapping
  is not evidence of lower host RSS.

# Historical Milestone 3 validation

- Host: Linux x86-64, Python 3.12.14, C compiler metadata in evidence/m3-linux-build.json.
- 22 M3 cases passed: 12 compiled-C input/environment cases, four assembly
  conformance checks, six interpreter memory/startup-limit rejection cases.
- All 16 non-fault cases matched QEMU 7.2.22 Linux user-mode on exact stdout,
  stderr and exit code. Rejection cases were not compared with Linux crash behavior.
- All 22 cases also passed with AddressSanitizer and UndefinedBehaviorSanitizer.
  Leak detection was disabled due to environment process-inspection limitations;
  no successful leak-check claim is made.
- All 20 M2 regression checks and 13 original Python unittest methods passed.
- Linux guest fixtures built independently with Zig 0.13.0/LLVM, including a
  freestanding O0 C program and assembly startup. Hashes/flags accompany the ELFs.
- M0/M1/M2 Mac acceptance passed per user report. M3 Mac acceptance is pending.
- Native ARM64 Linux hardware validation, general libc compatibility, heap syscall
  support, networking, isolation, JIT and Docker/VM benchmarks remain unverified.
- Historical M2 timings below refer to M2, not this expanded runtime version.

# Historical Milestone 2 validation

- Linux x86-64: compiled C backend builds with warnings treated as errors.
- 20 compiled-backend checks passed, including six independent ELF programs.
- AddressSanitizer/UndefinedBehaviorSanitizer: same 20 checks passed with leak
  detection disabled because the environment cannot inspect process tasks.
- Original Python suite: 13 tests passed.
- Seven measured samples per backend/workload; raw results in
  evidence/m2-linux-benchmark.json and build metadata in m2-linux-build.json.
- Linux median wall times: startup/exit Python 109.821 ms, C 6.886 ms;
  500,000-branch dispatch Python 684.651 ms, C 11.399 ms.
- These are synthetic host-local measurements. RSS was identical at the measured
  accounting floor; no memory improvement conclusion is supported.
- M0 and M1 Mac acceptance passed per user report. M2 Mac checks are pending.
- No native ARM64 Linux hardware check, JIT experiment or Docker/VM comparison.

# Prior Milestone 1 validation

- Host used here: Linux x86-64, Python 3.12.14.
- Compiler: Zig 0.13.0 (`zig cc`, LLVM assembler/linker).
- Six independently assembled and linked ARM64 Linux ET_EXEC binaries.
- Each binary was run by this runtime and QEMU 7.2.22 user-mode on Linux.
- All six matched exactly on stdout bytes, stderr bytes and exit status.
- 13 unittest methods passed (including the six-case integration method).
- readelf independently identifies the compiler-built ELF as AArch64 ET_EXEC,
  with separate read-only and executable LOAD segments.
- Source and binary hashes are in examples/independent/build-manifest.json.
- Detailed observed comparison results are in evidence/m1-linux-reference.json.

The earlier M0 hello and 12 tests passed on the user's Mac, per their report.
The M1 package subsequently passed on their Mac per user report. Native ARM64 Linux
hardware execution remains pending; QEMU user-mode is explicitly an emulated reference.
No Docker workload, isolation or performance claim is made.

One fixture development error (incorrect write length) and a subsequent exit
constant edit were caught by exact-output/status tests and corrected before this
release. The runtime execution implementation required no new instructions.

Reference binary used only during validation, not shipped or required on Mac:
https://deb.debian.org/debian/pool/main/q/qemu/qemu-user-static_7.2+dfsg-7+deb12u18+b3_amd64.deb

## M19 — bounded processes and basic shell (Apple Silicon Mac)

Implemented synchronous single-level interpreted children, rooted static execve,
wait4 status handling, shared execution limits, descriptor inheritance/CLOEXEC,
and a freestanding noninteractive guest shell. See docs/MILESTONES.md#m19 for the
restricted contract; no upstream ash or general process compatibility claim.

- `mac-m19-validation.json`: 39/39 checks passed, including OCI status and managed
  container start/restart/logs/removal.
- `evidence/m19-sanitizer.json`: 35/35 execution checks passed under ASan/UBSan.
  Direct M19 malformed-pointer/vector, argument-limit, clone-policy and wait-error
  checks also passed with sanitizers.
- M2 (20), M3 (22), M4 (17), lookup/decode cache conformance, and direct M18 ABI
  checks passed. Saved regression reports: M7–10 40/40, M11–17 30/30, M18 29/29.
- Python unit discovery: 44/45 passed outside the network-restricted sandbox.
  The unchanged `test_delayed_listener_counts_in_startup` benchmark test failed
  because readiness_attempts was 1 rather than greater than 1. Its elapsed-startup
  assertion passed. This is a timing/platform-sensitive test assumption; no
  benchmark code was changed for M19. Inside the sandbox, localhost binding and
  supervisor startup are prohibited, so those suites were run outside it.
- No independent Linux/QEMU M19 reference or production isolation audit performed.

## M20 — bounded pipes and descriptor redirection (Apple Silicon Mac)

Implemented nonblocking guest pipes, descriptor duplication, close-on-exec and
ordered shell redirection. Pipeline stages run sequentially with at most 1 MiB
buffered per pipe and stop on the first failing stage. This is not concurrent
streaming or POSIX pipeline-status compatibility. See docs/MILESTONES.md#m20.

- `mac-m20-validation.json`: 60/60 checks passed, including pipe capacity boundary
  and overflow, EOF, quoting and syntax rejection, ordered stderr duplication,
  read-only/traversal policies, temporary OCI writes, and managed-container append
  persistence across two starts.
- `evidence/m20-sanitizer.json`: 56/56 command/OCI checks passed under ASan/UBSan.
  Direct native checks also passed under ASan/UBSan, covering buffer/descriptor
  limits, EAGAIN/EPIPE, aggregate writev atomicity, duplicate/clone/exec ownership,
  reclamation, and file-write quotas through redirected descriptors.
- Saved regressions: M7–10 40/40; M11–17 30/30; M18 29/29; M19 39/39. M2 (20),
  M3 (22), M4 (17), both cache conformance checks and direct M18/M19 checks passed.
- All 18 OCI unit tests passed. The unrelated benchmark timing test noted in M19
  was not changed or rerun as part of M20 validation.
- Lifecycle/network acceptance ran outside the sandbox that blocks supervisor
  startup and localhost binding. No independent Linux/QEMU M20 reference or
  production isolation audit was performed.

## M21 — concurrent pipeline streaming (Apple Silicon Mac)

Implemented opt-in guest scheduling with four live children, blocking pipe and
wait4 suspension/retry, shared instruction bounds, and streaming pipelines.
M21 launches all stages and waits for all of them; it reports the first nonzero
status in command order. Default M19/M20 synchronous behavior remains available.
See docs/MILESTONES.md#m21 for the process, I/O and shell limits.

- `mac-m21-validation.json`: 72/72 checks passed, including exact 4 MiB output
  through two, three and four stages; a slow consumer; early reader exit; exec
  recovery; WNOHANG; live/lifetime child bounds; deadlock and CPU-budget failures;
  and two managed-container starts appending an exact combined 8 MiB.
- `evidence/m21-sanitizer.json`: 68/68 command/OCI checks passed under ASan/UBSan.
- The direct scheduler test passed under ASan/UBSan, with 6,198 context switches,
  1,807 reader suspensions and 773 writer suspensions. It verified every output
  byte and release of saved tasks, pipe buffers and filesystem descriptors.
  Output is in `evidence/m21-scheduler-native.txt`.
- Saved default-mode regressions passed: M7–10 40/40, M11–17 30/30, M18 29/29,
  M19 39/39 and M20 60/60. M2 (20), M3 (22), M4 (17), both cache-conformance suites,
  direct M18–20 checks and all 18 OCI unit tests passed.
- Lifecycle/network tests ran outside the sandbox that blocks supervisor startup
  and localhost binding. Independent Linux/QEMU validation, scheduled networking,
  interactive terminals, performance comparison and production isolation remain
  outside this milestone.

## M22 — scheduled guest cancellation (Apple Silicon Mac)

Implemented guest-only positive-PID cancellation for SIGINT, SIGKILL, SIGPIPE
and SIGTERM, signal-0 existence checks, signal wait statuses and shell status
translation. See docs/MILESTONES.md#m22 for the bounded scope.

- `mac-m22-validation.json`: 30/30 checks passed. The same 30 checks passed under
  ASan/UBSan in `evidence/m22-sanitizer.json`.
- Five direct native sanitizer scenarios passed, including blocked readers and
  writers, CPU-bound children, root cancellation during wait and self-termination.
  They verify release of saved task memory, pipe buffers and filesystem resources.
  Output is saved in `evidence/m22-native.txt`.
- `evidence/m22-linux-reference.json` records matching results from the same
  portable ELF on the interpreter and ARM64 Linux through the existing pinned
  Docker reference image. It covers default termination by signals 2/9/13/15,
  raw wait statuses and absence of reaped PIDs. This is validation only, not a
  VM fallback in the runtime. Broader signal/pipe semantics are not certified.
- Saved regressions passed: M18 29/29, M19 39/39, M20 60/60 and M21 72/72, plus
  the M7–10 and M11–17 suites. M2–4, both cache-conformance suites, direct M18
  checks and direct M19–21 sanitizer checks also passed.
- Production corpus artifact selection, terminal support, full signal ABI and
  independent production confinement acceptance remain open. M22 is not fully
  accepted, and M23–M80 are not implemented or accepted by these tests.

## Continued implementation through the M27–M32 work areas (2026-09-21)

M23: 19 terminal/process-group checks, including PTY input, EOF, foreground Ctrl-C,
OCI forwarding and unchanged host terminal attributes. M24: 89 local checks and
26 Linux comparisons for unmodified upstream ash, process ABI and ISA probes.
M25: 25 local/sanitizer checks and five Linux comparisons for signal frames and
job control. M26: 29 normal/lifecycle/Linux checks, 19 sanitizer checks, five
native cleanup probes, and the 30-check existing HTTP/product suite under the
scheduled network path.

M27 has 19 working-directory/directory-cursor checks in normal and sanitizer builds, plus a Linux directory-lifetime comparison. M28 has
26 checks including five Linux comparisons, 21 local sanitizer checks and native
mapping-protection/error tests. M29 has 13 HTTP/OCI/lifecycle checks in normal and
sanitizer builds; the same application suite passes with the experimental macOS
client-only host sandbox. These are scoped candidates with documented gaps.

M30 replays eight Linux matches, two declared filesystem policy differences and
five saved malformed/resource rejections. Its classifier has four unit tests;
a hosted CI workflow is configured but has not been observed running. M31 has
11 allocation/budget checks in normal and sanitizer builds, persistent-storage
native checks, two admission tests, and two native host-syscall sandbox probes.
M32 records 16 standalone process CPU/RSS/time samples without a Docker/VM/battery
comparison or savings claim.

Recent regression reports are under `evidence/m31-*.json`: filesystem/lifecycle,
HTTP/API/Compose, BusyBox, pipelines, cancellation, terminal control, signal frames,
and working directories. Reports contain the tested runtime hashes where supported;
earlier hashes are historical results, not a claim that every report used the
latest source revision. `docs/MILESTONE-STATUS.md` records remaining acceptance
gaps. No evidence here closes independent review, signing/clean-Mac qualification,
production stacks, or M80.

The historical readiness-test timing assumption was corrected: the elapsed startup
assertion remains, while a blocked first connect is permitted to succeed without
a retry. Deterministic retry behavior remains covered by its separate unit test.

The timer ABI probe passed five local/sanitizer checks and four Linux comparisons.
The corrected full Python unit suite passed 51/51 tests with loopback fixtures
permitted (`evidence/m31-python-tests.txt`).

## Dockiva integration — 2026-09-21

The separate Dockiva checkout now implements an opt-in VM-free backend. `scripts/verify_dockiva.py` passed all three VMFree tests, including real-engine create/list/inspect/start/stop/restart/snapshot-logs/remove against a temporary M19 image store (`evidence/dockiva-integration-tests.txt`). Dockiva `go test ./...` passed (`evidence/dockiva-go-tests.txt`); the frontend TypeScript check and production build passed. The build emitted macOS deployment-target linker warnings and a frontend bundle-size warning. No signed app, interactive GUI session or clean-Mac installation was qualified. Milestone acceptance gates remain unchanged.

## Bundled Dockiva engine and startup — 2026-09-21

Built a self-contained Apple Silicon engine helper with PyInstaller 6.22.3 and bundled it into Dockiva. Default-settings and bundled-engine startup/reuse/demo-execution/restart tests passed using the helper inside the ad-hoc-signed app with PATH limited to /usr/bin:/bin (`evidence/dockiva-app-bundle-tests.txt`). Full Go tests passed and the frontend production build passed. Ad-hoc signature verification passed. New Apple Silicon installs default to VM-free; explicit selection starts it, while existing choices and disabled launch startup remain preserved. The test verifies the launch path and bundled dependency execution, not an interactive GUI session or older-macOS/clean-Mac qualification.

## Dockiva persistent HTTP demo — 2026-09-21

The bundled-engine integration test passed seeded image discovery, HTTP response validation, remaining Running, Stop closing the listener, Start and Restart (`evidence/dockiva-http-tests.txt`). The frontend build passed. The updated helper includes both shell and HTTP fixtures. The HTTP fixture remains bounded by existing instruction/CPU limits; no production milestone is marked complete.

## Dockiva capacity and API recovery — 2026-09-21

Twenty isolated bundled-engine checks passed: four concurrent HTTP containers, fifth-runtime admission rejection, released-slot reuse, API shutdown/restart with workloads preserved and rediscovered, token rotation, recovered Stop/Restart and cleanup. Short steady windows measured 44.3 MiB summed RSS / 0.09% CPU for the idle engine and 189.8 MiB / 0.38% for the engine plus four idle servers and supervisors. GUI/WebViews are excluded; host RAM is 32 GiB. See `docs/DOCKIVA-INTEGRATION.md#dockiva-8gb-validation` and `evidence/dockiva-capacity.json`. No 8 GB hardware or production gate is marked accepted.

## Database compatibility probes — 2026-09-21

Pinned cached PostgreSQL 16.13, Redis 7.4.2 and Valkey 9.0.0 ARM64 binaries were
probed with complete transitive ELF library fixtures. Linux reference version
commands passed; VM-free version probes were blocked. PostgreSQL dynamic loading
reported libstdc++ allocation errors and exhausted the bounded instruction budget;
Redis/Valkey exceeded the 16 MiB executable limit. See
`docs/DATABASE-COMPATIBILITY-PROBES.md` and `evidence/database-probes.json`.
No database service performance or production acceptance is claimed.
