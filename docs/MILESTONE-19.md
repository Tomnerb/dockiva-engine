# M19: bounded guest processes and a basic noninteractive shell

M19 adds interpreted child processes, static ELF replacement, Linux wait-status
packing, and a small freestanding ARM64 shell. The shell is new fixture code;
this does **not** establish BusyBox ash or general POSIX shell compatibility.
The existing unmodified-source M18 BusyBox executable supplies external commands.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m19_native.py
python3 scripts/verify_m19.py --lifecycle --report mac-m19-validation.json
python3 oci.py run examples/m19/shell-image -- -c 'busybox echo hello; probe exit 7; status'
python3 oci.py run examples/m19/shell-image -- -c 'false && busybox echo skipped || busybox cat /hello.txt'
```

## Process contract

- ARM64 `clone(SIGCHLD, 0, 0, 0, 0)` copies registers, stack, ELF memory, brk and
  anonymous mappings. The interpreter executes the child synchronously, then
  resumes the parent with a synthetic PID. No host child process, Linux kernel,
  JIT or VM is used. Parents cannot run concurrently with children.
- There are at most 16 children over the entire invocation, with one child level.
  Nested clone returns EAGAIN. Thread/shared-memory flags, alternate stacks and
  TID/TLS arguments return EOPNOTSUPP. A child cannot create grandchildren even
  after exec. Consequently launching another shell as a child does not enable
  nested external commands.
- Parent and children share the original instruction budget and the existing
  file-write/create counters. At most 16 successful execs are allowed across
  the invocation. Exec and clone never replenish these bounds. Runtime faults or
  instruction exhaustion abort the entire invocation with status 125; they are
  not translated into simulated child signals. Existing bounded blocking I/O
  remains; this milestone does not introduce a total wall-time deadline.
- Regular-file descriptors have separate tables but share host open-file offsets,
  as with fork. Closing a child descriptor leaves its parent's descriptor valid.
  Clone with open directory or socket descriptors returns EOPNOTSUPP. Child-opened
  descriptors are closed on child completion. Standard output/error are inherited;
  stdin, descriptor duplication/redirection and pipes remain unsupported.
- `execve` opens a regular executable through the existing root-relative no-follow
  traversal. It supports the existing ARM64 static ET_EXEC format only. It stages
  and validates the new image before replacing the old image, returning ENOEXEC
  on invalid ELF without destroying the caller. Missing files, denied traversal,
  bad pointers and oversize arguments return guest errors. No host executable is
  launched and no shebang or dynamic loader is supported.
- argv must contain 1–64 entries; envp supports 0–64 entries. Strings share a
  64 KiB budget. Guest pointers are checked before image replacement. Successful
  exec resets CPU, heap and mapping state and preserves regular descriptors except
  those marked close-on-exec. The filesystem now tracks guest O_CLOEXEC and
  F_GETFD/F_SETFD independently of host descriptor protection. Exec with sockets
  open is rejected.
- `wait4` accepts a positive child PID or -1, options 0 or WNOHANG, an optional
  writable four-byte status pointer, and null rusage. Children have already
  completed when clone returns, so WNOHANG returns a completed child immediately.
  Normal exit is packed as `(code & 255) << 8`. Invalid status pointers do not
  consume the result; a second wait returns ECHILD. getpid/getppid expose synthetic
  identities (parent 1, children 2–17), never host identities.

These are intentionally restricted process semantics. Memory copies are bounded
by the existing address-space limits and single child level; this is not a new
production isolation boundary or a comprehensive aggregate memory quota.

## Shell contract

`/bin/sh -c COMMAND` accepts up to 4,096 input bytes, 32 parsed commands and 32
arguments per command. It parses the whole input before executing anything.
Whitespace, single/double quotes, backslash escapes, `;`, `&&` and `||` are supported.
Builtins are `true`, `false`, `exit [0–255]` and `status` (prints the last status).
The final command's status becomes the shell exit status; `exit` without an
argument uses the previous status. Missing commands return 127 and exec failures
return 126; syntax errors return 2. Exhausting the child limit returns 125.

Absolute executable paths are used directly; other names are prefixed with
`/bin/`. There is no PATH search. Use `busybox echo ...`, `busybox cat ...`, etc.
The fixture's `probe` command exists for validation of statuses and syscalls.

Expansion (including `$?`), globbing, substitutions, redirects, pipelines,
background jobs, scripts from files/stdin, terminal control and interactive input
are outside this milestone. Unsupported unquoted syntax is rejected; quoting and
escaping provide literal arguments within the documented small parser.

## Build and evidence

Guest rebuilding requires Clang's AArch64 target and LLVM ld.lld (the script can
also locate the linker bundled with Rust). The host runtime needs only its usual
C11 compiler. No guest compiler is needed to run the bundled fixtures.

```sh
python3 scripts/build_m19.py --cc /path/to/clang --ld /path/to/ld.lld
python3 scripts/build_native.py --sanitize
python3 scripts/verify_m19_native.py --sanitize
python3 scripts/verify_m19.py --native build/vmfree-c-sanitize
```

The manifest under examples/m19 hashes the shell, probe, startup, syscall helper,
guest ELFs and reused BusyBox ELF. The OCI fixture includes BusyBox; its existing
GPL source/license bundle and provenance remain under third_party/busybox and
examples/m18.

Mac validation is recorded in `mac-m19-validation.json`. Sanitizer and earlier
milestone regression reports are under `evidence/m19-*.json`. Linux/QEMU reference
comparison and upstream shell compatibility have not been run for M19. Pipes and
interactive terminals remain separate steps.
