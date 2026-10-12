# Combined Milestones 7–10

M7–M10 build on the Mac-validated M6 image loader. The release includes a
root-relative file adapter, private writable container trees, a conventional
static musl-linked program, and a local lifecycle CLI. There is no Linux VM.

## Acceptance on your Mac

Extract the complete ZIP into a fresh directory. Python 3.10+ and the local C
compiler are required. Linux guest binaries and OCI layouts are already bundled.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m7_10.py --report mac-m7-10-validation.json
python3 scripts/verify_m6.py
python3 scripts/verify_m5.py
python3 scripts/verify_m4.py
python3 scripts/verify_m3.py
python3 scripts/verify_m2.py
```

Expect 40 combined PASS lines, followed by 24, 14, 17, 22 and 20 from the previous
milestones. The combined test uses temporary state and cleans up its instances.
Mac execution has not been run here; the user supplies that acceptance result.

## M7: read files from an image

```sh
python3 oci.py run examples/m7_10/http-image
```

In another terminal, within 30 seconds:

```sh
curl --noproxy '*' --max-time 10 http://127.0.0.1:18080/
```

The response contains `Hello from the image filesystem!`. The guest reads
`/www/index.html` through Linux openat/read/close and constructs the HTTP header.
The server handles one request and exits. Append `-- auto` to use an ephemeral
port, then use the printed port for curl. Foreground `oci.py run` uses a temporary,
read-only guest filesystem; its extraction is removed on exit.

The C CLI now accepts `--root DIRECTORY` and optional `--writable`. Image commands
supply roots automatically. Avoid pointing writable execution at important host
directories: use the lifecycle tool's private image copies.

## M8: independent writable state

```sh
python3 containers.py create counter-a examples/m7_10/counter-image
python3 containers.py create counter-b examples/m7_10/counter-image
python3 containers.py start counter-a
python3 containers.py inspect counter-a
python3 containers.py logs counter-a
```

After `inspect` shows `exited`, start counter-a again. Its log now contains `1`
then `2` on separate lines. Starting counter-b produces `1`; its filesystem is
independent. Logs append across restarts. The original OCI blobs are unchanged.

Each container gets a full private copy of the merged image, not a copy-on-write
or overlay mount. State persists after exit/stop and is removed by `rm`.
`create NAME IMAGE --read-only` disables guest writes. `--arg VALUE` can be repeated
to replace image Cmd. Host state defaults to `~/.vmfree/containers`, or use
`--state-dir DIRECTORY` before the action. The state directory must be owned by
the current user and have permissions 0700.

## M9: an ordinary static musl program

```sh
./build/vmfree-c run examples/m9/libc-demo.elf
python3 oci.py run examples/m7_10/libc-image
```

The first prints `static musl OK`. The second prints `hello`, read from its image.
`examples/m9/libc-demo.c` uses standard headers, main, strcmp, open, read, write,
close and errno. It is linked normally against Zig's musl with its real startup
code; there is no custom entrypoint or vmfree syscall wrapper in this program.

Four application cases (startup, file reading, ENOENT and invalid arguments),
plus an independent assembly instruction probe, match QEMU user-mode on Linux.
The reference file-read invocation uses an equivalent host file path because the
interpreter maps guest `/` to its root; QEMU itself is not a rootfs sandbox.

New execution support includes logical operations/masks, bitfields, extended
addition, multiply/high multiply, conditional compare, TPIDR_EL0 storage, and a
small set of vector byte duplication/register-transfer/load/store operations
used by musl. DCZID reports that DC ZVA is prohibited. This is not general SIMD,
floating-point, all-musl, malloc, stdio, dynamic-linker or thread compatibility.
Linux set_tid_address returns a single guest thread ID; no thread creation,
futex waiters, clear-on-exit notification or guest signal delivery is implemented.

## M10: lifecycle

```sh
python3 containers.py create web examples/m7_10/http-image --arg auto
python3 containers.py start web
python3 containers.py list
python3 containers.py inspect web
python3 containers.py logs web
python3 containers.py logs web --stderr
python3 containers.py stop web
python3 containers.py rm web
```

`start` is detached and confirms process launch, not application readiness.
Use the server's `PORT` log line as readiness. A supervisor owns each child,
records its exit status, and holds an exclusive lifecycle lock. Starting twice
or removing a running instance fails. `stop` is idempotent after normal exit.
Stopping a running guest terminates its interpreter; the recorded status is 143
for SIGTERM, or 137 if a forced kill is needed after three seconds.

The supervisor listens on an ephemeral IPv4 loopback control port. Stop requests
require a random 256-bit token stored in the private state directory; it is not
printed by inspect or logs. This avoids using a stored PID as authority to kill
a process. Unauthenticated stop requests are tested and ignored. Guest TCP
listeners and supervisor control endpoints have separate purposes.

No automatic reboot recovery or application restart policy is provided. If a
supervisor is killed abruptly, stale running state blocks start/removal and needs
manual inspection; do not assume the guest has exited. Do not manually edit state
while an instance is running. `rm` intentionally deletes that instance's files
and logs. This is a CLI with per-instance supervisors, not a Docker API daemon.

## Filesystem contract and limits

Supported Linux ARM64 calls:

| Call | Scope |
|---|---|
| openat (56) | Root-relative or an opened guest directory; regular files/directories |
| read/write (63/64), close (57) | Separate file descriptor table; sockets retain M5 behavior |
| fstat (80) | Explicit 128-byte ARM64 layout; synthesized ownership/mode/time fields |
| lseek (62) | SET, CUR, END for regular file descriptors |
| ftruncate (46) | Bounded length changes in writable mode |
| mkdirat (34), unlinkat (35) | Directory creation, file deletion, empty directory deletion |
| renameat (38) | Rename within one private root |

Open flags supported: access mode, CREAT, EXCL, TRUNC, APPEND, DIRECTORY,
NOFOLLOW, CLOEXEC and LARGEFILE with explicit ARM64-to-host translation. Other
flags fail. Absolute paths refer to the guest root; relative AT_FDCWD paths use
that root too. Guest chdir/getcwd, directory enumeration, links, chmod/chown,
mounts, devices, file-backed mmap and filesystem permissions/metadata fidelity
are not implemented. fstat uses virtual mode/ownership and zero timestamps;
it must not be treated as a host stat structure.

Every ancestor is opened relative to a directory descriptor with O_NOFOLLOW;
`..`, links, non-ASCII names, backslashes and colons are rejected. Path handling
is a restricted normalized subset rather than complete Linux resolution.
Only trusted images and private trees produced by this loader are in scope.
Concurrent host mutation, host-created hard links, local state tampering and
hostile same-user processes are outside this prototype's boundary.

File descriptors: 32 slots (guest IDs 64–95), separate from 32 socket slots.
Per file: 16 MiB. Per read/write: 1 MiB. Per execution: cumulative writes and
requested truncate sizes total at most 64 MiB; at most 4096 create-open/mkdir
operations. These are conservative operation budgets, not aggregate persistent
disk quotas. Budgets reset on restart. Managed execution uses 10 million
instruction steps. M4 memory and M5 socket/time limits remain unchanged.
Logs are not rotated or assigned disk quotas. No production isolation claim.

## Evidence and reproduction

The 40 combined checks passed on Linux x86-64. The first 39 also passed with
AddressSanitizer and UndefinedBehaviorSanitizer; the final control-authentication
check is Python supervisor behavior. Leak detection was disabled because this
environment cannot inspect process tasks. Previous M2–M6 suites passed.
Reports are under `evidence/m7-10-*`. Native ARM64 Linux hardware remains untested.

```sh
python3 scripts/build_m7_10.py --zig /path/to/zig
python3 scripts/verify_m7_10.py --qemu /path/to/qemu-aarch64-static
```

The reference command is Linux-only. Zig 0.13.0 builds the bundled fixtures;
source/binary hashes and exact build commands are recorded. The normal Mac
workflow needs neither Zig nor QEMU. See `third_party/NOTICE.md` for musl licensing.

References:

- [Linux generic stat ABI](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/stat.h)
- [Zig 0.13.0 bundled musl](https://github.com/ziglang/zig/tree/0.13.0/lib/libc/musl)
