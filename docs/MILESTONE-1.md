# Milestone 1: independently built Linux ELF probes

The six supplied programs are assembled and linked by Zig 0.13.0's LLVM toolchain.
They do not use `examples/build_hello.py` or runtime code when building the ELF.
They are small assembly programs, not general C/Go applications. This milestone
tests loader and syscall behavior; it does not expand the runtime's instruction set.

## Mac check (no compiler or Docker needed)

Extract this complete package into a fresh directory, then:

```sh
cd vmfree-runtime
python3 scripts/verify_m1.py --report mac-m1-report.json
python3 -m unittest discover -s tests -v
```

Expect six PASS lines. `Linux reference: not-run` is expected on this command;
it does not pretend to perform Linux reference execution on your Mac.
`mac-m1-report.json` records the host, ELF hashes, exact bytes and exit codes.

## What each probe verifies

| Probe | Expected stdout | Expected stderr | Exit |
|---|---|---|---:|
| stdout | Independent ARM64 Linux probe + newline | Empty | 0 |
| stderr_exit42 | Empty | Independent ARM64 Linux probe + newline | 42 |
| write_return | Independent ARM64 Linux probe + newline | Empty | 30 |
| zero_write | Empty | Empty | 0 |
| bad_fd | Empty | Empty | 247 (-EBADF truncated to 8 bits) |
| exit_group | Empty | Empty | 42 (298 truncated to 8 bits) |

The build manifest binds binaries to source using SHA-256. These hashes detect
accidental changes; they are not a signature or a guarantee of authenticity.

## Rebuild independently

With Zig 0.13.0 available on the host (Mac or Linux):

```sh
python3 scripts/build_independent.py --zig /absolute/path/to/zig
python3 scripts/verify_m1.py
```

The script uses `zig cc -target aarch64-linux-musl -nostdlib -static` and writes
the exact flags, compiler version and hashes into `build-manifest.json`.
No Linux VM is needed for cross-compilation. Do not regenerate the M0 fixture to
validate these probes; they use a separate build path.

## Reference on ARM64 Linux hardware

Copy the same extracted package to an ARM64 Linux machine:

```sh
chmod +x examples/independent/*.elf
python3 scripts/verify_m1.py --native-linux --report arm64-linux-m1-report.json
```

The script compares stdout, stderr and status from our interpreter with direct
execution of the exact same binary on that Linux machine. The platform guard
rejects this option on macOS and x86 Linux. Hardware execution remains a distinct
acceptance gate even if an emulated reference passes.

## Optional reference on x86 Linux

If qemu-aarch64 user-mode is installed:

```sh
python3 scripts/verify_m1.py --qemu /usr/bin/qemu-aarch64 --report qemu-m1-report.json
```

This reference uses QEMU instruction translation and the host Linux kernel.
It is labeled `qemu-user-linux`, not native ARM64 hardware validation. QEMU is only
a test oracle here; it is not invoked by the runtime and is not a Mac dependency.

## Remaining scope

M1 Mac acceptance was subsequently reported successful by the user (all six
probes and the test suite). Native ARM64 Linux hardware comparison remains pending.
There is no Docker image support, process isolation or performance claim.

References:
- [Zig toolchain](https://ziglang.org/)
- [QEMU user-mode documentation](https://www.qemu.org/docs/master/user/main.html)
