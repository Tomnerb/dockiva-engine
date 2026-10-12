# M20: bounded pipes and descriptor redirection

M20 extends the small guest shell with pipelines and file/descriptor redirection.
Pipeline stages still execute synchronously inside the interpreter. This supports
bounded command output, not concurrent streaming or an interactive terminal.
The M19 fixtures remain unchanged; use the new M20 image below.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m20_native.py
python3 scripts/verify_m20.py --lifecycle --report mac-m20-validation.json

python3 oci.py run examples/m20/shell-image -- -c 'busybox echo hello | busybox cat'
python3 oci.py run examples/m20/shell-image --writable -- -c 'busybox echo alpha >/out; busybox echo beta >>/out; busybox cat </out'
python3 oci.py run examples/m20/shell-image --writable -- -c 'probe both >/combined 2>&1; busybox cat /combined'
```

`oci.py run --writable` per mits writes only in that invocation's temporary guest
root, which is discarded after exit. Without it, OCI runs remain read-only.
Managed containers retain their existing private writable roots across starts.
There is no new host bind-mount or filesystem escape mechanism.

## Shell behavior and limits

- `|` connects one command's stdout to the next command's stdin. Each producer
  finishes before its consumer starts. At most **1 MiB** can be buffered per pipe.
  Commands that need concurrent consumers or produce more buffered output require
  a later scheduler/streaming milestone.
- A pipeline stops at the first stage that returns a nonzero status and uses that
  status. If every stage succeeds, it returns zero. This differs from the POSIX
  convention of reporting the final stage's status and avoids hiding an upstream
  buffer-capacity failure behind a successful consumer. `&&` and `||` apply to
  the whole pipeline.
- `< file`, `> file`, and `>> file` read, truncate/create, and append/create.
  Optional descriptor prefixes are 0, 1 or 2. `2>file`, `2>>file`, `2>&1`, `1>&2`,
  `<&0`, and `>&-` are examples. Other numeric descriptor prefixes are rejected.
- Redirections apply from left to right after pipeline endpoints are attached.
  `probe both >/out 2>&1` puts both streams in the file; `probe both 2>&1 >/out`
  leaves stderr on the original stdout. Explicit file redirection can override a
  pipeline endpoint.
- Quoting and escapes protect literal operators and filenames with spaces. The
  entire command is parsed before execution, so malformed syntax cannot partially
  execute earlier commands. Runtime redirection failures return 1; earlier valid
  redirections in the same command can already have created/truncated files.
- Builtins used in pipelines or with redirections run in children. `exit` in a
  pipeline exits only that stage; standalone `exit`, including with redirection,
  exits the shell. `status` prints and retains the previous status, as in M19.
- Guest stdin defaults to EOF. The host's interactive input is not exposed.
  Standard descriptors are restored when a child finishes; redirects do not leak
  into subsequent commands or change the runtime's own diagnostic streams.
- Existing limits remain: 4,096 command-text bytes, 32 parsed commands, 32 arguments
  per command, 16 children and 16 successful execs per invocation, one child level,
  and a shared instruction budget. Each command accepts at most eight redirects.

No background jobs, command substitution, expansion, globbing, here-documents,
PTYs, terminal control, interactive mode, BusyBox ash or POSIX shell compatibility
is claimed. M19's `true`, `false`, `status`, and `exit` builtins remain available;
external bare executable names resolve under `/bin/`.

## Runtime descriptor contract

The new adapter implements ARM64 `pipe2`, `dup`, `dup3`, and bounded
`fcntl(F_DUPFD/F_DUPFD_CLOEXEC)` allocation. Standard descriptors 0–2 and extended
slots 96–127 belong to this adapter; the existing socket and filesystem descriptor
ranges retain their previous numbering. Duplication targets are limited to 0–2
and 96–127; allocating duplicates starts at 96. Duplicating sockets or directories
is rejected. These are scoped descriptor operations, not a fully unified Linux
file table.

`pipe2` requires O_NONBLOCK and optionally O_CLOEXEC. Blocking mode is explicitly
rejected, since no concurrent process scheduler exists. There are at most eight
live pipes, each backed by a 1 MiB guest buffer. Clone shares these buffers and
retains endpoint references; close/exec/child completion release references.
The last endpoint close frees the buffer. Guest descriptor operations never close
or replace the host's standard descriptors.

Pipe reads return buffered bytes, EAGAIN when empty with writers present, and EOF
when empty after all writers close. Writes return EPIPE when no readers remain;
no guest SIGPIPE is delivered. A full buffer returns EAGAIN. Writes up to 4,096
bytes, including aggregate writev payloads, either fit completely or return
EAGAIN; larger writes may be partial. Reads/writes and writev totals are capped at
1 MiB per call, with at most 16 iovecs. Pipe fstat reports FIFO type; lseek returns
ESPIPE. F_GETFD/F_SETFD and F_GETFL are supported; changing pipe blocking mode,
pipe polling and readv remain outside this adapter.

Regular-file duplicates preserve shared offsets and pass through the existing
read-only policy and per-file/aggregate write limits. They support read, write,
writev, fstat, lseek and the documented fcntl operations. Other filesystem
operations on these aliases are not newly supported. Close-on-exec applies to
new descriptors and redirected standard descriptors; dup/dup3 without O_CLOEXEC
clears the new descriptor's close-on-exec flag. Failed duplication preserves its
target. Root-relative traversal, symlink rejection, executable validation and
M19's process restrictions remain active. This milestone does not close the
production isolation gate.

## Build and validation

Guest builds use Clang's AArch64 target and LLVM ld.lld, as in M19. Mac users can
run the bundled ELFs without a guest compiler:

```sh
python3 scripts/build_m20.py --cc /path/to/clang --ld /path/to/ld.lld
python3 scripts/build_native.py --sanitize
python3 scripts/verify_m20_native.py --sanitize
python3 scripts/verify_m20.py --native build/vmfree-c-sanitize
```

The build manifest hashes the guest sources and ELFs plus the reused M18 BusyBox
ELF. BusyBox's existing source/license bundle remains under third_party/busybox.
`mac-m20-validation.json` records Mac command and container acceptance;
`evidence/m20-sanitizer.json` records ASan/UBSan execution checks. Direct native
checks cover buffer and descriptor exhaustion, EOF/EAGAIN/EPIPE, writev atomicity,
failed duplication, clone/exec ownership, reclamation, and redirected-file quotas.
Regression reports for M7–19 are saved under `evidence/m20-regression-*.json`.
No independent Linux/QEMU M20 comparison or production isolation audit was run.
