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
