# M39: glibc startup work in progress

The direct ELF loader accepts opt-in input bounds of 16–64 MiB and mmap arenas of
16–128 MiB through `--elf-limit-mib` and `--mmap-limit-mib`. Defaults remain 16 MiB.
ELF load segments and individual guest lookups retain their prior bounds. All
allocations remain charged to the aggregate allocator limit. Input allocation
now uses the actual regular-file length plus one byte to detect length changes.

Startup supplies a bounded auxiliary vector with synthetic identity, zero optional
CPU capability masks, executable name and 16 fresh host-generated random bytes.
The vector has space for maximum argument/environment counts. It does not expose
host pointers. `verify_m39_native.py --sanitize` checks its layout and lifetime.

Descriptor-based `fstatat(AT_EMPTY_PATH)` supports glibc library metadata queries.
Historical MAP_DENYWRITE/MAP_EXECUTABLE flags are ignored, while mapping bounds,
private-only mappings and W^X policy remain enforced. Optional set_robust_list and
rseq registration, madvise and prctl explicitly return ENOSYS; no robust mutex or rseq support is
claimed. The read-only prlimit64 stack query reports the real fixed 1 MiB guest
stack. Changes to limits and other resources remain unsupported.

See the database probe report for actual application outcomes. M39 remains open:
threading prerequisites, locale/NSS, full images and application behavior have not
passed qualification. These native-only options are not enabled in Dockiva.

Database startup follow-up implements realtime/monotonic clock queries and
resolution, gettimeofday with a UTC guest kernel timezone, a read-only affinity
mask for the single guest CPU, and bounded getrandom reads (at most 256 bytes per
call, ordinary and NONBLOCK modes). Darwin uses its kernel CSPRNG; Linux uses
getrandom directly. Other random modes and clock IDs are rejected. Native
sanitizer checks cover output bounds, partial-length behavior, error paths and
fresh output; they do not certify a cryptographic construction.

`--uid`/`--gid` select one mapped identity (default 0). Get-ID syscalls, stat
ownership and auxiliary-vector identity agree. Reasserting the current identity
with setuid/setgid succeeds; changing identity is rejected. Host credentials never
change, and multiple guest owners/users are not implemented. Per-task umask,
regular-file/directory creation modes and chmod are covered by M44 native checks.
The process launcher now supports scheduled vfork with a shared image and parent
suspension until exec/exit; this does not implement general pthreads.
