# M25 — bounded ash job control and caught signals

`--job-control` enables interactive input, process trees, caught guest signals,
stop/resume and foreground-group ownership. It uses the pinned upstream M24 ash:

```sh
python3 scripts/build_native.py
python3 scripts/verify_m25.py --report mac-m25-validation.json
python3 oci.py run examples/m24/shell-image --job-control --max-steps 100000000 -- -i
```

Run `cat`, press Ctrl-Z, inspect `jobs`, try `bg`, then `fg`. Background terminal
reads stop the guest group with SIGTTIN. Ctrl-C terminates the foreground job and
leaves the shell usable. `exit` closes the session. The existing four-live-child
and sixteen-lifetime-child/exec limits still apply; this is a bounded session.

Guest signal frames preserve general registers, NZCV flags, masks and SIMD
registers; `rt_sigreturn` validates the frame before restoring guest state.
Malformed frames terminate the offending guest with SIGSEGV, preserving its
parent. Default/ignored dispositions, caught handlers with explicit restorers,
SA_SIGINFO, SA_RESTART, SA_NODEFER and SA_RESETHAND have scoped implementations.
Interrupted pipe reads return EINTR or restart according to SA_RESTART. Exec
resets caught handlers. Child notifications, stopped/continued wait statuses,
orphan reparenting and foreground process-group changes are integrated.

Frame offsets follow the Linux ARM64
[sigcontext ABI](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/include/uapi/asm/sigcontext.h)
and [signal entry/return implementation](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c).
The independent assembly/C probe validates register modification through ucontext,
SIMD restoration, bad-frame isolation, stop/continue and blocked-read handling.

The verifier exercises actual upstream ash traps and a real PTY session covering
jobs/bg/fg, Ctrl-Z, background-read stop, foreground resume and Ctrl-C. Host
terminal attributes remain unchanged after exit. Separate Linux comparisons run
the exact probe ELF in the pinned reference image; reports record actual outcomes.

Limits: no alternate signal stack, SVE, floating-point status changes, realtime
signal queues, full session management, full termios mutation or general terminal
emulation. Host canonical settings remain in force; upstream line editing can
echo input twice. No raw-mode/full-screen program support is claimed. Unsupported
signal flags/operations return explicit errors. M37 must broaden and qualify the
signal ABI alongside threads. Production security acceptance remains open.
