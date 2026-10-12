# M22 — scheduled guest cancellation

Implementation tested on Apple Silicon macOS. Full M22 acceptance remains open:
the production application corpus is not yet pinned. An independent ARM64
Linux comparison passed for default child termination, raw wait statuses and
reaped PID absence. This change is a bounded default-termination
implementation, not full POSIX signal support.

## Implemented behavior

With `--schedule`, ARM64 syscall 129 (`kill`) accepts positive synthetic guest
PIDs. Signal 0 probes existence; SIGINT (2), SIGKILL (9), SIGPIPE (13) and SIGTERM
(15) request default termination. Guest PIDs and signals never reach a host
`kill` API. Self-signalling stops execution before the next guest instruction.
Other tasks receive cancellation at their next scheduler selection, including
tasks suspended on pipe reads/writes or `wait4`.

Cancellation releases the task's memory and descriptors, updates pipe reference
counts and makes its raw signal number available in `wait4` status. The M22
shell reports `128 + signal`, including pipeline failures and conditionals.
Cancelling one child preserves its parent and siblings. Terminating the root
ends the invocation and releases its remaining children, as with normal root exit.

A zombie PID exists until reaped; signalling it succeeds without altering its
saved exit status. A reaped or unknown PID returns ESRCH. Invalid signal numbers
return EINVAL; unsupported signals, process groups and calls without `--schedule`
return EOPNOTSUPP. SIGKILL takes precedence over an earlier pending termination;
otherwise the first queued supported termination is retained.

## Reproduction

```sh
python3 scripts/build_native.py
python3 scripts/build_m22.py
python3 scripts/verify_m22.py --report mac-m22-validation.json
python3 scripts/verify_m22_native.py --sanitize
python3 oci.py run examples/m22/signal-image --schedule --max-steps 10000000 -- cancel-read
python3 oci.py run examples/m22/shell-image --schedule -- -c 'probe self-term; status'
```

The first OCI command prints `cancelled child; sibling and parent survived` and
exits 0. The shell command prints `143` and exits 143. Guest sources, ELFs and
the BusyBox fixture are hashed in `examples/m22/build-manifest.json`.

## Evidence and limits

The command verifier covers CPU-bound children, blocked pipe readers/writers,
root termination during a blocked wait, self-signals, zombie/reap behavior,
invalid/unsupported requests, scheduler-disabled behavior, shell pipelines,
conditionals and OCI execution. Cache-disabled variants exercise the same cases.
The direct native sanitizer verifier checks that blocked operations actually
suspended and all task, pipe and filesystem resources were released, including
root termination with a live child.

Reports: `mac-m22-validation.json`, `evidence/m22-sanitizer.json`,
`evidence/m22-native.txt`, `evidence/m22-linux-reference.json` and
`evidence/m22-regression-*.json`. There are 30 command checks in each of the normal
and sanitizer reports. Five direct sanitizer scenarios verify resource cleanup.

The Linux comparison uses the exact same portable probe ELF in the interpreter
and an existing ARM64 Linux image pinned by image ID, with networking disabled,
read-only mounts and bounded resources. `scripts/verify_m22_reference.py` records
the image/runtime/probe hashes and exact outputs. Docker is a test reference
only. Blocked-pipe scheduling and cancellation have local tests, not independent
Linux equivalence evidence; Linux pipe capacities and scheduling differ.

Existing bounds remain: four live children, sixteen lifetime children/execs,
single-level process relationships and one shared guest instruction budget.
No handlers, masks, alternate stacks, signal frames, stop/continue, process
groups, terminal Ctrl-C forwarding or host-to-guest signal bridge are implemented.
EPIPE still returns an error without automatically delivering SIGPIPE. Scheduled
networking remains unsupported. A passing cancellation suite does not establish
production isolation or support for arbitrary images.

The [corpus registry](ACCEPTANCE-CORPUS.json) pins current prototype fixtures and
lists the production targets still requiring exact artifacts and workload sizes.
The [execution ledger](MILESTONE-STATUS.md) retains the remaining M1–M80 gates,
including total RAM, CPU and battery comparison requirements.
