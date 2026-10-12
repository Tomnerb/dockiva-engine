# Milestone 4: bounded guest allocation

The compiled C backend adds raw Linux ARM64 syscalls brk (214), munmap (215),
and mmap (222). All guest memory is interpreter-owned data. These calls do not
invoke a Linux kernel or create host executable mappings.

## Mac check

Extract into a fresh directory. Requires Python 3.10+ and a local C compiler.
The independently compiled Linux fixtures are included.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m4.py --report mac-m4-validation.json
python3 scripts/verify_m3.py
python3 scripts/verify_m2.py
```

Expect 17 M4 PASS lines, 22 M3 PASS lines and 20 M2 PASS lines.
`Linux reference: not-run` is expected for Mac verification.

```sh
./build/vmfree-c run examples/m4/demo.elf
```

Expected stdout: `heap allocation OK` followed by a newline. Exit status: 0.
The guest grows and writes to its brk heap, shrinks it, maps two pages, copies
text across a page boundary, writes that text through the syscall adapter, and
unmaps the memory. It is freestanding C using raw syscalls, not libc malloc/free.

## Supported contract

| Operation | Implemented behavior |
|---|---|
| brk(0) | Return the current break |
| brk(address) | Grow/shrink within an 8 MiB range following the page-aligned ELF end |
| brk failure | Preserve and return the previous break, following the raw Linux syscall convention |
| mmap | Private anonymous mappings only, flags exactly 0x22; choose an address in a fixed 16 MiB arena |
| mmap protections | Explicit PROT_NONE, READ, WRITE, or READ|WRITE; no executable mappings |
| mmap length | Nonzero, rounded up to 4 KiB; bounded by available arena pages |
| munmap | Page-aligned start inside the arena, nonzero length rounded to pages; partial ranges and holes allowed |
| Reallocation | Freed mapping pages can be reused; new mappings are zero-filled |
| Access checking | Deny accesses to unmapped pages or pages without the requested permission |

The mmap arena begins at guest address 0x1000000000. ELF segments overlapping it
are rejected. Non-fixed address hints are currently ignored. Anonymous fd is
ignored; offsets must be page-aligned, and offset zero is the intended use.
Shared, file-backed, fixed-address and other flags return -EOPNOTSUPP (95).
Executable or unknown protection bits return -EINVAL (22). No mprotect exists.
These unsupported-feature error choices are prototype policy, not full Linux
behavior. PROT_WRITE does not implicitly grant read access in this interpreter.

munmap outside the arena returns -EOPNOTSUPP, including ELF, stack and brk ranges;
it does not implement Linux's ability to unmap arbitrary address-space regions.
Invalid alignment/zero length returns -EINVAL. Oversized or exhausted mmap requests
return -ENOMEM (12) without altering existing mappings. The allocator uses first
fit and needs a contiguous free run; total free pages alone do not ensure success.

The logical break is tracked separately from page-rounded accessibility. A shrink
within a page preserves that page, while regrowing released whole pages produces
zero-filled memory. There is no ASLR. The fixed brk range must fit below the stack
and avoid the mmap arena.

## Host memory and limits

Backing arrays are allocated lazily: up to 8 MiB for brk and 16 MiB for mmap.
Guest release changes accessibility and permits reuse; backing arrays are retained
until runtime exit rather than immediately returned to the host allocator.
These caps are separate from the earlier 16 MiB ELF segment cap and 1 MiB stack.
No reduction in Mac RSS or Docker memory use is claimed.

Accesses crossing pages within the arena are checked against every touched page.
The existing limitation on accesses spanning separate ELF/stack/heap regions
remains. The write adapter prevalidates the full buffer; Linux partial writes
across a faulting range are not emulated.

## Verification

17 independent C fixtures cover:

- Ten Linux-comparable cases: allocation demo, brk shrink/regrow, partial unmapping,
  mapping reuse/zero fill, zero-length mmap, invalid munmap, read-only zero pages,
  PROT_NONE allocation, unmapping holes, and rejecting a break below its base.
- Three runtime-policy cases: 16 MiB mapping exhaustion/recovery, 8 MiB break
  limit, and unsupported mapping/protection flags.
- Four interpreter faults: writing read-only memory, reading after unmapping,
  reading PROT_NONE, and reading a whole page released by brk shrink.

The ten comparable cases match QEMU user-mode on Linux on exact output and exit
status; mapping addresses themselves need not match. Fault and cap policies are
not presented as Linux conformance. M4 Mac acceptance and native ARM64 Linux
hardware comparison remain pending.

```sh
# Optional reference on Linux with QEMU user-mode:
python3 scripts/verify_m4.py --qemu /path/to/qemu-aarch64 --report linux-m4.json

# On native ARM64 Linux:
chmod +x examples/m4/*.elf
python3 scripts/verify_m4.py --native-linux --report arm64-m4.json

# Rebuild fixtures with Zig 0.13.0/LLVM:
python3 scripts/build_m4.py --zig /absolute/path/to/zig
```

The fixtures use a constrained freestanding O0 build with integer registers only,
without libc, compiler runtime checks or stack-protector helpers. Source hashes,
ELF hashes and exact flags are recorded. The runtime build metadata now hashes
both C source and the new heap header.

Python remains the earlier-instruction reference; new allocation behavior is
implemented in the C backend and validated using independent Linux execution.
This milestone does not establish general malloc/libc, dynamic linking, threads,
networking, Docker image compatibility or security isolation.

Implementation references:
- [Linux mmap/munmap manual](https://man7.org/linux/man-pages/man2/mmap.2.html)
- [Linux brk manual and raw syscall return behavior](https://man7.org/linux/man-pages/man2/brk.2.html)
