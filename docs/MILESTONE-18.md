# M18: static BusyBox command compatibility

This candidate runs a minimal five-applet BusyBox 1.37.0 build from unmodified
upstream sources. It is statically linked for ARM64 Linux against musl using Zig
0.13.0. The applet code uses -O0 and general registers; selected musl routines
still use SIMD and exclusive instructions. This is a configured upstream build,
not a stock distribution's full BusyBox executable or proof of general image
compatibility. Shells, process creation, pipes and threads remain unsupported.

## Validate on Mac

The archive includes the guest binaries and OCI layout. Rebuild only the host C
runtime; no Linux VM, BusyBox compiler or Docker rebuild is needed to use them.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m18_native.py
python3 scripts/verify_m18.py --report mac-m18-validation.json
```

Try the OCI fixture directly:

```sh
python3 oci.py run examples/m18/busybox-image -- echo 'Hello from BusyBox'
python3 oci.py run examples/m18/busybox-image -- cat /hello.txt
python3 oci.py run examples/m18/busybox-image -- pwd
python3 oci.py run examples/m18/busybox-image -- ls /bin
```

The verification suite also creates a temporary managed container, runs BusyBox
mkdir in its private writable root, restarts it to confirm persistence/error
status, reads stderr logs, and removes the instance. Existing user containers and
images are not used by the tests. `cat` file arguments are supported; stdin/TTY
and arbitrary BusyBox flags/applets are not part of this milestone.

## Runtime changes

- Signed byte/halfword/word loads, integer division, widening multiply-add/subtract,
  variable shifts/rotate, bit/byte reversal and leading-zero/sign counts.
- The exact SIMD zero-immediate form used by musl.
- Single-thread 32/64-bit exclusive load/store and CLREX. This is an interpreter
  reservation model; it does not add threads, futexes or interprocess atomics.
  Stores/syscalls invalidate reservations. Instruction fetch still checks current
  bytes and executable permissions through the existing caches.
- getcwd reports `/` for a configured guest root. No chdir or host cwd exposure.
- Root-relative newfstatat for regular files/directories, with explicit guest ABI
  packing. Symlinks and special files are rejected even with AT_SYMLINK_NOFOLLOW;
  this intentionally differs from Linux's full metadata semantics.
- getdents64 translates host directory entries into aligned Linux records. It
  preserves continuation across buffers, validates guest pointers, limits each
  buffer to 1 MiB and each enumeration to 4,096 entries, and rejects non-ASCII or
  overlong names. Directory cookies are synthetic; only rewind-to-zero is supported.
- Limited file-descriptor fcntl operations needed by musl, non-TTY ioctl responses,
  and writev with at most 16 vectors / 1 MiB total. Regular-file vectors use the
  existing bounded write path and do not promise atomic multi-vector writes.

Existing root-relative no-follow traversal and read-only policy remain active.
There is still no production OS sandbox, and concurrent host tampering remains
outside the current threat model. More syscalls mean more review surface; this
release does not close the security or clean-Mac distribution gates.

## Evidence and scope

The bundled Linux report includes native execution and independent QEMU/Linux
checks for the new instruction probe and all five selected commands. QEMU pwd
expects its real test-directory path while the runtime expects `/`; this deliberate
namespace difference is recorded in the verifier. No Linux VM is used by the
runtime itself. QEMU is optional validation tooling and is not bundled.

Additional direct tests cover directory ABI records, small-buffer continuation,
rewind/close, invalid pointers/flags, symlink/traversal rejection and writev bounds.
Tests also cover both existing caches disabled individually. Prior M2, M3, M4,
M7–10 and M11–17 suites and both cache-conformance tests passed locally. The M18
suite passed with ASan/UBSan, with leak detection disabled in this environment.
Actual Mac compilation and M18 execution are pending the commands above.

The preceding Mac memory-lookup and decode-cache benchmarks are preserved under
evidence. They establish improvements for the earlier HTTP fixture, not BusyBox
performance or performance of every later runtime build.

## Source and rebuilding

Complete upstream source is bundled at
`third_party/busybox/busybox-1.37.0.tar.bz2`, alongside its GPLv2 license. Its SHA256
is pinned in scripts/build_m18.py. The build configuration, compiler adapter,
conformance assembly and fixture hashes are under examples/m18. Existing musl and
Zig license notices are under third_party.

Rebuild guest fixtures on Linux with Python 3.12+, GNU make, a host C compiler and
Zig 0.13.0:

```sh
python3 scripts/build_m18.py --zig /path/to/zig
```

The compiler adapter translates BusyBox dependency-file flags for Zig and omits
unsupported linker diagnostics/map/sort options; it does not edit BusyBox sources.
Build timestamps/toolchain paths can affect byte identity; the manifest records
the resulting artifact hashes. `--package-only` regenerates the OCI layout and
manifest from existing guest binaries. A rebuild is not required for Mac users.

Upstream source: https://busybox.net/downloads/busybox-1.37.0.tar.bz2
