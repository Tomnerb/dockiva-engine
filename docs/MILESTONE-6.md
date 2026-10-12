# Milestone 6: local OCI image execution

This release adds `oci.py`, a Python standard-library adapter that validates a
local OCI image layout, merges supported layers in memory, extracts a private
temporary tree, and passes the selected ELF to the existing C interpreter.
The HTTP implementation is still the Linux guest program from M5.

## Mac acceptance

Extract the complete release into a fresh directory, then:

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m6.py --report mac-m6-validation.json
python3 scripts/verify_m5.py
python3 scripts/verify_m4.py
python3 scripts/verify_m3.py
python3 scripts/verify_m2.py
```

M6 expects 24 PASS lines: 18 loader unit tests and six integration checks. These
cover digest/DiffID integrity, extraction, invalid paths and links, whiteouts,
opaque directories, replacement, case collisions, size/member limits, config and
platform errors, inspect, two HTTP responses, Cmd/Env, Cmd override, and stopping
with cleanup. Existing regression counts are 14, 17, 22 and 20 respectively.
The M6 suite is adapter validation, not comparison with another OCI runtime.

```sh
python3 oci.py inspect examples/m6/http-image --ref demo
python3 oci.py run examples/m6/http-image
```

The server prints `PORT 18080`; within 30 seconds, in another terminal:

```sh
curl --noproxy '*' --max-time 10 http://127.0.0.1:18080/
```

Expected body: `VM-free HTTP server OK` plus a newline. It exits after one request.
Append `-- auto` to the run command to choose an ephemeral port instead.

```sh
python3 oci.py run examples/m6/args-image
python3 oci.py run examples/m6/args-image -- 999
```

The first prints `sum=15` and `MODE=oci`, exiting 15. The second prints `sum=27`
and `MODE=oci`, exiting 27. Nonzero statuses here are intentional guest results.
Arguments after `--` replace image Cmd and retain Entrypoint. No shell is invoked.
The native CLI's new `--argv0 NAME` preserves the image executable's guest name.

## Accepted subset

- Local layout version 1.0.0, index schema 2, exactly one selected OCI image
  manifest. Use `--ref` to select the `org.opencontainers.image.ref.name` annotation.
- Linux/ARM64 config and, when supplied, matching descriptor platform.
- SHA-256 blob digest and size verification, plus uncompressed layer DiffIDs.
  Digests detect inconsistent content; they do not authenticate the publisher.
- Uncompressed tar and gzip tar layers; regular files and directories. Empty
  regular-file whiteouts and opaque directory markers remove lower-layer content
  before entries from their own layer are applied.
- Printable ASCII paths, no traversal, absolute paths, backslashes, colons,
  duplicate paths within a layer, or case collisions. Links, sparse files,
  devices and FIFOs are rejected. Ownership, timestamps and extended attributes
  are not reproduced. Files become 0600 or 0700 according to executable bits;
  directories become 0700.
- Absolute ELF entrypoint, Entrypoint/Cmd arrays and explicit Env. No guest PATH
  lookup or host environment import. Default command must itself be valid even
  when Cmd will be overridden. Full ELF validation remains in the C loader.
- Empty or `/` WorkingDir only; no User or Volumes settings; default/SIGTERM
  StopSignal only. This does not implement guest filesystem or identity semantics.
- SIGTERM/SIGINT forwarded to the host interpreter process, with temporary-tree
  cleanup after exit. This does not implement Linux guest signal handlers.

Limits: 1 MiB JSON, 32 MiB per stored blob, 32 layers, 64 MiB total expanded tar
bytes, 16 MiB per file, 4096 tar members and merged paths, 1024-character paths,
32 path components, 64 argv entries including argv0, 64 environment entries,
and 64 KiB combined argument/environment strings. Existing native limits apply.
Errors detected by the adapter exit 125; interpreter errors retain its status.
`inspect` verifies the image and reports its command; it does not execute or fully
validate the ELF instruction stream.

## Boundaries

Use trusted bundled images. This is image packaging support, not a secure
container boundary or a conforming OCI runtime. The temporary rootfs is a source
of the executable; guest filesystem syscalls and rootfs mediation remain future
work. No chroot, Linux namespaces, resource isolation or identity changes occur.
Local layout files must not be modified concurrently during loading; symlink
checks are not a race-resistant filesystem capability system. Abrupt SIGKILL or
host failure can leave temporary files.

No registries, image pulls, Docker save archives, nested indexes, zstd layers,
dynamic linker, general libc, Docker API or arbitrary Linux-image compatibility.
The Python adapter adds startup overhead; no Docker/VM performance comparison
has been performed. M6 passed on Linux x86-64 here; Mac acceptance remains pending.
M5's 14 checks and HTTP response passed on the user's Mac per their report.

## Reproduction and references

`python3 scripts/build_m6.py` regenerates both deterministic OCI layouts from the
bundled M3/M5 ELF fixtures. Their original sources, compiler flags and hashes are
included under those milestones. No Linux toolchain is needed to rebuild the
packaging or run the Mac checks.

OCI specifications consulted for this implementation:

- [Image layout](https://github.com/opencontainers/image-spec/blob/main/image-layout.md)
- [Image configuration](https://github.com/opencontainers/image-spec/blob/main/config.md)
- [Filesystem layers](https://github.com/opencontainers/image-spec/blob/main/layer.md)
