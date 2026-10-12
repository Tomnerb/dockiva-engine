# M23 — foreground input and guest process groups

Implemented and tested on Apple Silicon macOS with real pseudo-terminals.
`--interactive` enables scheduling, reads the inherited standard input, and
forwards host Ctrl-C to the selected synthetic guest foreground group. The
default invocation still gives guests EOF on stdin.

The runtime never changes host terminal attributes, descriptor blocking flags,
or host foreground process groups. Canonical line editing and EOF therefore use
the host terminal's existing settings. Terminal input suspends a guest without
blocking runnable siblings. Waiting for human input does not consume guest
instructions. The instruction budget still bounds executing guests.

The opt-in group model supports getpgid/setpgid, kill of an explicit negative
group ID or the caller's group, TIOCGPGRP/TIOCSPGRP, and TIOCGWINSZ. Synthetic group
IDs never reach a host process-group API. After the foreground group exits, the
remaining parent can reclaim guest foreground ownership. SIGTERM to the host
runtime requests root termination and existing root-exit cleanup.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m23.py --report mac-m23-validation.json
python3 oci.py run examples/m23/signal-image --interactive --max-steps 10000000 -- echo-input
python3 oci.py run examples/m23/shell-image --interactive --max-steps 10000000 -- -c 'busybox cat'
```

Enter a line, then Ctrl-D on an empty line to finish, or Ctrl-C to terminate.
This is foreground input support, not an interactive command prompt.

Nineteen checks passed normally and under ASan/UBSan. They cover input/EOF,
Ctrl-C to the root, Ctrl-C to a child foreground group while the parent survives,
OCI forwarding, terminal preservation after success/signals/budget failure,
invalid ioctl pointers/groups, redirected stdin, cache-disabled group execution
and default EOF. Reports are `mac-m23-validation.json` and
`evidence/m23-sanitizer.json`; M21/M22 regressions also passed.

Limits: one guest session, no setsid, stop/resume, jobs/fg/bg, terminal mode
changes, background-read SIGTTIN or background-write SIGTTOU. Background reads
fail with EIO. The group adapter is deliberately limited and is not full POSIX
job control. Nested process relationships, post-exec setpgid restrictions and
signal handlers require further work. M22's production-corpus acceptance remains
open; this local implementation does not close that prerequisite.
