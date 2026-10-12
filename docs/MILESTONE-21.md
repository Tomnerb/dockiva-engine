# M21: concurrent pipeline streaming and backpressure

M21 adds opt-in scheduling for guest processes. Pipeline stages can now make
progress together, so a stream can exceed the pipe buffer's 1 MiB capacity.
Guest children remain inside the C interpreter; no Linux VM or host process is
created for each guest child. M19/M20 fixtures retain their default synchronous
behavior when `--schedule` is omitted.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m21_native.py
python3 scripts/verify_m21.py --lifecycle --report mac-m21-validation.json

python3 oci.py run examples/m21/shell-image --schedule --max-steps 100000000 -- -c 'busybox echo hello | busybox cat'
python3 oci.py run examples/m21/shell-image --schedule --max-steps 100000000 --writable -- -c 'probe large | busybox cat >/out; busybox ls /out'
```

`probe large` emits 4 MiB of `Z` bytes. To display the complete stream, run
`probe large | busybox cat`; tests compare all output bytes rather than only the
length. `probe slowcat` deliberately spends guest instructions between reads to
exercise producer backpressure.

`--max-steps` sets the invocation's shared instruction budget. OCI's default
remains 100,000, which is insufficient for many larger pipelines. The examples
explicitly request a larger finite budget. `--writable` retains M20's behavior:
writes affect a temporary private root and are discarded when an OCI run exits.

For a persistent managed container:

```sh
python3 containers.py create stream examples/m21/shell-image --schedule --max-steps 100000000 --arg=-c --arg='probe large | busybox cat >>/out'
python3 containers.py start stream
python3 containers.py inspect stream
```

The scheduler and budget settings are saved in that container's configuration.
Each start appends to its private `/out`, subject to the existing file-size and
write limits. Existing container configurations default to synchronous execution
and their original 10,000,000-step budget. Docker/Compose APIs have not gained a
new scheduling option in this milestone.

## Scheduling and process limits

- The interpreter uses round-robin slices of up to 4,096 guest instructions.
  Every process consumes the same global step budget, including syscall retries.
  A CPU loop cannot monopolize guest execution or acquire a fresh budget by exec.
- Up to four children may be live concurrently, plus the root process. The existing
  lifetime limits of sixteen children and sixteen successful execs remain. Nested
  child creation, threads and shared-address-space clone variants are rejected.
  The M21 shell rejects pipelines with more than four stages before execution.
- Scheduling saves CPU registers, flags, stack/ELF/heap/mapping state, TLS and SIMD
  registers, filesystem descriptors and enumeration state, and redirected/pipe
  descriptors. Lookup caches and exclusive reservations are invalidated when a
  context is restored. Pipe buffers and regular-file offsets remain shared through
  their existing descriptor ownership rules.
- Blocking pipe read/write/writev and blocking wait4 suspend the calling guest.
  It is retried after pipe or child state changes. Runnable guests continue using
  the same host interpreter thread; this is concurrency, not parallel host CPUs.
- wait4 with WNOHANG returns zero while a matching child is still live, and a
  completed child PID/status when available. Blocking waits retain the guest's
  arguments across suspension; errors do not consume a completed child's status.
- Exiting children release their memory and descriptors immediately and leave a
  waitable status. Root exit cancels remaining guest children and releases their
  resources; detached/background processes are not supported.
- If every remaining process is blocked and no recorded state change can wake one,
  the invocation fails with status 125 and a deadlock diagnostic. Budget exhaustion
  and interpreter faults also fail the whole invocation with status 125.

Address-space, descriptor and pipe limits remain bounded. With multiple live
images, memory consumption is higher than M20; this is not a new aggregate-memory
quota or production isolation guarantee. Socket creation is rejected in scheduled
mode. Pipe polling, general asynchronous host I/O, host-stdout backpressure,
threads and guest signal delivery remain separate work.

## Streaming and status behavior

With `--schedule`, pipe2 accepts blocking pipes as well as O_NONBLOCK pipes.
Buffers remain capped at 1 MiB, with at most eight live pipes. Empty blocking
reads suspend while writers remain; full blocking writes suspend while readers
remain. Readers receive EOF after the last writer closes and buffered data drains.
Writers receive EPIPE when all readers close; there is still no simulated SIGPIPE.
Nonblocking endpoints retain M20's EAGAIN behavior.

F_GETFL/F_SETFL can query/change a pipe endpoint's O_NONBLOCK state in scheduled
mode. Duplicated/inherited references share that endpoint setting. Small writes
and aggregate writev payloads up to 4,096 bytes retain the all-or-wait behavior;
larger writes may return partial progress. No bytes are replayed when a syscall
suspends after zero progress. M20's per-call I/O limits remain unchanged.

The M21 shell launches every stage, closes the parent's unused endpoints, and
waits for every stage. Redirections still apply left to right after pipe setup.
The pipeline returns the first nonzero stage status in command order, or zero if
all stages succeed. **A failed stage does not prevent already-launched stages
from running**, unlike M20's sequential stop-at-failure behavior. This retains
visibility of producer failures without claiming POSIX final-stage status rules.

Quoting, `<`, `>`, `>>`, standard-descriptor duplication, and M20's parser and
builtin limits remain. Subshells, nested external shells, interactive input,
terminals, job control and general POSIX/BusyBox ash compatibility remain open.

## Build and evidence

```sh
python3 scripts/build_m21.py --cc /path/to/clang --ld /path/to/ld.lld
python3 scripts/build_native.py --sanitize
python3 scripts/verify_m21_native.py --sanitize
python3 scripts/verify_m21.py --native build/vmfree-c-sanitize
```

Guest sources, ELFs and the reused BusyBox ELF are hashed in the M21 manifest.
The existing BusyBox source/license bundle is unchanged. Mac command/container
results are in `mac-m21-validation.json`; sanitizer results and legacy-mode
regressions are under `evidence/m21-*.json`.

The native scheduler test transfers 4 MiB through three stages with a slow middle
stage, checks every byte, asserts actual reader/writer suspensions and context
switches, and verifies task/pipe/file resource release. Its output is recorded in
`evidence/m21-scheduler-native.txt`. Command tests also cover four-stage streaming,
early reader exit, exec recovery, WNOHANG, live/lifetime child limits, root-exit
cleanup, CPU-budget exhaustion, deadlock detection, redirection and persistence.
No independent Linux/QEMU M21 reference, performance comparison or production
isolation audit has been performed.
