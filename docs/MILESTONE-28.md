# M28: bounded ARM64 musl dynamic loading

`--dynamic` enables PT_INTERP loading from the guest root, AT_BASE/AT_ENTRY/AT_PHDR
startup values, private file-backed guest mappings, fixed mappings confined to
the guest mmap arena, and mprotect. Host memory is never made executable.
Writable/executable guest mappings and ELF segments are rejected. Mapping errors
are checked before replacing old contents. Segment protection changes invalidate
the lookup cache. Exec failures preserve the calling image.

The pinned upstream fixture is Alpine 3.22.1 ARM64, digest
`sha256:4bcff63911fcb4448bd4fdacec207030997caf25e9bea4045fa6c8c44de311d1`.
BusyBox and musl bytes are unmodified. Library aliases are materialized as regular
files because the root adapter still rejects links. This is not full Alpine
image compatibility. `scripts/build_m28.py` records image and file hashes.

Validation: 26 checks in `mac-m28-validation.json`, including five Linux
comparisons; 21 local sanitizer checks in `evidence/m28-sanitizer.json`; mapping
bounds/protection/atomic-failure assertions in `scripts/verify_m28_native.py`.
The separate DSO probe exercises actual constructors, symbol relocation and TLS.
The Linux reference uses the kernel's normal executable/PT_INTERP entry path.
Standalone loader invocation of that probe is not in the validated contract
(the initial Linux reference attempt exited with SIGBUS). BusyBox shell scripts,
nested exec, pipelines, relative files and invalid interpreter rejection pass.
The 89-check static-shell suite also passes after this work.

Limits: 16 MiB ELF segment budget, 16 MiB mmap arena, 128 segments, private mappings
only, no general overlapping-load-segment support, no shared mappings, no general
SIGBUS delivery for accesses beyond a mapped file's EOF, and no glibc/JIT target.
Fixed mappings outside the mmap arena are rejected (including musl's optional
brk guard request). Broader loader and malformed-image qualification remains
open; these results do not imply arbitrary dynamic image support.
