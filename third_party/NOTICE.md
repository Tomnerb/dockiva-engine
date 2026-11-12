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

M18 includes a minimal configured BusyBox 1.37.0 binary from unmodified upstream sources (GPLv2). Complete source and license are under busybox/; build configuration and compiler adapter are under examples/m18/ and scripts/build_m18.py. musl licensing applies to the statically linked libc. The M18 busybox image and the M19–M23 shell and signal images carry the same binary as `bin/busybox`.

# FEX bundle (linux/amd64 images)

`fex/fex-emu-armv8.4_2610-1~n_arm64.deb` is FEX 2610 (MIT), unmodified, as the
fex-emu PPA built it for Ubuntu 24.04:
https://launchpad.net/~fex-emu/+archive/ubuntu/fex. Its source:
https://github.com/FEX-Emu/FEX/tree/FEX-2610. `scripts/build_x86.py` unpacks
`FEX`, `FEXServer` and FEX's x86 vDSO (`libVDSO-guest.so`, 64- and 32-bit) from it,
with the arm64 glibc they load from Ubuntu 24.04's
libc6 (LGPL-2.1+) and gcc-14 runtime packages (GPL-3 with the GCC Runtime Library
Exception), into the engine bundle's `_internal/build/x86`. That bundle's `share/`
holds each package's copyright file, and `share/SOURCES` each package's SHA-256 and
the Launchpad page of its source.

# BusyBox and musl from public images (M24, M28, M29)

These binaries are copied unmodified from pinned public images. Each fixture's
`build-manifest.json` records the image digest and the files' SHA-256.

- `examples/m24/busybox.elf`, also `bin/busybox`, `sh`, `cat` and `echo` in
  `examples/m24/shell-image`: `/bin/busybox` of the Docker Official Image
  `busybox@sha256:fc6dddc4c44b1bfe37f41cae8e67d1693828e8f42a91862816d7953e2c9d3f23`
  (arm64). BusyBox 1.37.0, GPL-2.0-only. Source: the BusyBox 1.37.0 release under
  `busybox/`; the image's build files are at https://github.com/docker-library/busybox.
- `examples/m28/root` (`bin/busybox`, `sh`, `cat`, `echo`, `lib/ld-musl-aarch64.so.1`,
  `lib/libc.musl-aarch64.so.1`) and the same files in `examples/m29/fetch-image`:
  from the Alpine Linux image
  `alpine@sha256:4bcff63911fcb4448bd4fdacec207030997caf25e9bea4045fa6c8c44de311d1`.
  BusyBox 1.37.0 (GPL-2.0-only) and musl 1.2.5 (MIT, `musl-COPYRIGHT`). Their
  sources, with Alpine's patches, are the `main/busybox` and `main/musl` packages
  of https://gitlab.alpinelinux.org/alpine/aports.

The GPL-2.0 text is in `busybox/LICENSE`.

# Redis preview image (examples/redis-preview)

`scripts/build_redis_preview.py` copied these files unmodified into
`examples/redis-preview/image` from a `redis` 7.4.2 image for linux/arm64 based on
Debian 12 (image ID `sha256:3b0f54bf27a77c98d319dc3a6706ed500a8985309f0b6c6407599b869f4a2bd9`):

| Files | Component | License | Source |
|---|---|---|---|
| `usr/local/bin/redis-server` | Redis 7.4.2 | RSALv2 or SSPLv1, at your choice: `redis-LICENSE.txt` | https://github.com/redis/redis/tree/7.4.2 |
| `lib/aarch64-linux-gnu/libc.so.6`, `libm.so.6`, `lib/ld-linux-aarch64.so.1` | GNU C Library, Debian 2.36-9+deb12u9 | LGPL-2.1-or-later: `LGPL-2.1.txt` | https://snapshot.debian.org/package/glibc/2.36-9%2Bdeb12u9/ |
| `lib/aarch64-linux-gnu/libssl.so.3`, `libcrypto.so.3` | OpenSSL 3.0.15, Debian 12 package | Apache-2.0: `../LICENSE` | https://github.com/openssl/openssl/tree/openssl-3.0.15 |

Redis 7.4 is source-available, not open source: the RSALv2 or SSPLv1 terms, not
this repository's Apache-2.0 terms, apply to `redis-server`. It also contains the
libraries under `deps/` of the Redis source (jemalloc, Lua, hiredis and others),
each under the permissive license kept there.
