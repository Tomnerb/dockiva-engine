# Bundled static musl fixture

`examples/m9/libc-demo.elf` is built by Zig 0.13.0 with its bundled musl libc.
The application source is included. Rebuild with `scripts/build_m7_10.py`.
The musl copyright/license text is in `musl-COPYRIGHT`.

The linked AArch64 memset implementation carries:
Copyright (c) 2012-2020, Arm Limited. SPDX-License-Identifier: MIT.
The MIT terms included in `musl-COPYRIGHT` apply to this implementation too.

Zig's sources and bundled library sources:
https://github.com/ziglang/zig/tree/0.13.0/lib/libc/musl

Zig compiler runtime helpers linked into the freestanding guest fixtures are
covered by the Zig contributors MIT license in `zig-LICENSE`.

M18 includes a minimal configured BusyBox 1.37.0 binary from unmodified upstream sources (GPLv2). Complete source and license are under busybox/; build configuration and compiler adapter are under examples/m18/ and scripts/build_m18.py. musl licensing applies to the statically linked libc.

# FEX bundle (linux/amd64 images)

`fex/fex-emu-armv8.4_2610-1~n_arm64.deb` is FEX 2610 (MIT), unmodified, as the
fex-emu PPA built it for Ubuntu 24.04:
https://launchpad.net/~fex-emu/+archive/ubuntu/fex. Its source:
https://github.com/FEX-Emu/FEX/tree/FEX-2610. `scripts/build_x86.py` unpacks
`FEX` and `FEXServer` from it, with the arm64 glibc they load from Ubuntu 24.04's
libc6 (LGPL-2.1+) and gcc-14 runtime packages (GPL-3 with the GCC Runtime Library
Exception), into the engine bundle's `_internal/build/x86`. That bundle's `share/`
holds each package's copyright file, and `share/SOURCES` each package's SHA-256 and
the Launchpad page of its source.
