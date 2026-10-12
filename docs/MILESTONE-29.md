# M29: first practical application candidate

The candidate is a bounded HTTP artifact fetcher using unmodified Alpine 3.22.1
BusyBox wget and musl, packaged in `examples/m29/fetch-image`. It downloads from
IPv4 loopback HTTP to stdout or a writable guest file. Configuration uses normal
wget arguments. DNS, HTTPS, proxies, external networks and the full Alpine image
remain outside this candidate's contract. No runtime VM fallback is used.

Implementation adds validated vectored reads and guest ITIMER_REAL/SIGALRM
handling. Alarms belong to guest tasks, use monotonic deadlines, are not inherited
by fork, and preserve the existing signal-frame mechanism. Timers have millisecond
resolution and a one-day bound; CPU timer kinds are explicitly unsupported. The scheduler
checks them while tasks are blocked. Managed configuration can now retain dynamic
loading, scheduled networking and job-control/signal flags across restarts.

`mac-m29-validation.json` and `evidence/m29-sanitizer.json` cover normal and slow
bodies, chunking, redirects, 404, truncated responses, a one-second timeout,
graceful guest SIGTERM, forced host termination, OCI stdout, two managed starts
with persisted output, and removal. Files survive restart in private container
state. A killed download is not promised to be atomically committed.

This is a scoped application candidate. M27's remaining filesystem gaps and
M31's confinement/aggregate-limit review remain prerequisites for acceptance.

The timer probe adds five local/sanitizer checks and four Linux comparisons for
invalid inputs (including timer installation before an old-value copy fault),
fork inheritance, periodic handlers, default SIGALRM status and prompt host
termination while a guest is blocked with a long alarm.

The Linux timer reference uses `docker --init` so the probe is an ordinary process.
Linux namespace-init default-signal immunity is not implemented for the runtime's
synthetic root PID; that remains a container-semantics compatibility gap.
