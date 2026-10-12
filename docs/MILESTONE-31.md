# M31: confinement and aggregate limits (in progress)

The interpreter now budgets all allocations made through its allocator across
all guest tasks. The default is 256 MiB, configurable with `--memory-limit-mib`
(1–1024). Counters include allocation headers and report current/peak bytes and
denials with `--profile`. These are allocation counters, not RSS or RAM savings.
A failed clone rolls back staged images and duplicated descriptors, returning
ENOMEM without corrupting the parent or leaking pipe references.

The managed state store serializes start admission and reserves each active
runtime's configured allocation budget. It admits at most four runtimes and
1024 MiB of reservations. Reservations follow supervisor-held locks and release
on exit. Other state stores, direct native invocations, libc allocations and
supervisor/OS memory are outside this accounting. Existing host CPU, descriptor,
core-file and file-size limits remain in the managed launcher.

Guest roots are scanned through descriptors before execution, with 128 MiB of
regular-file contents, 8192 entries and depth 32 bounds. Write/truncate growth
shares a counter across guest tasks. Deletion/shrink does not reclaim accounting
until the next run, which is conservative. Restart rescans persisted contents.
This does not impose an aggregate disk quota across all state stores.

On macOS, `--host-sandbox` adds an experimental default-deny Seatbelt policy via
`confined_exec.py --seatbelt`. It allows the explicit executable/guest root,
required system libraries, standard I/O and outbound loopback TCP. Root writes
require `--writable`. Explicit bind/listen operations and host fork/other exec are
denied. Nonstandard inherited descriptors are closed before sandbox entry.
This is a **client-only qualification candidate**, not the server sandbox.
The attempted localhost listener filter admitted wildcard listeners; that design
was rejected and all explicit binds were removed from the candidate policy.

The host-syscall test bypasses all guest adapters and checks outside-file read/write,
symlink/hardlink escapes, root write policy, outbound connections, explicit binds,
fork and exec. Both read-only and writable probes pass on this Mac, and the M29
HTTP/error/timeout/shutdown/OCI/managed-restart suite passes under the sandbox.
The installed macOS dyld-support profile informed startup directory permissions;
Apple identifies that interface as private and subject to change. See also
[Chromium's Seatbelt documentation](https://chromium.googlesource.com/chromium/src/+/main/sandbox/mac/)
for the distinction between policy testing and independent review.

Evidence: `mac-m31-validation.json`, `evidence/m31-sanitizer.json`,
`mac-m31-sandbox-validation.json`, `evidence/m31-sandbox-fetch.json`,
`tests/test_m31_capacity.py`, and `scripts/verify_m31_native.py`.

M31 remains OPEN: independent review, server-capable host confinement, cross-store
aggregate limits, filesystem races, broader OS/hardware qualification and abuse
campaigns are not complete. The opt-in policy does not make legacy/default runs
safe for arbitrary untrusted images. M33 public preview remains blocked.
