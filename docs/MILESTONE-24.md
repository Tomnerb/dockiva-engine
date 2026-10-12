# M24 — bounded scripts with unmodified upstream ash

The executable is copied verbatim from the public Linux ARM64 BusyBox 1.37.0
musl image. Its image digest, platform image ID and ELF hash are recorded in
`examples/m24/build-manifest.json`. It is not the reduced M18 BusyBox build.
Docker extracts the test fixture and provides optional Linux references; the
runtime itself executes the ELF without a VM fallback.

```sh
python3 scripts/build_native.py
python3 scripts/verify_m24.py --report mac-m24-validation.json
python3 oci.py run examples/m24/shell-image --process-tree --max-steps 10000000 -- -c 'echo "$(echo upstream | cat)"'
python3 oci.py run examples/m24/shell-image --process-tree --writable --max-steps 10000000 -- -c 'echo alpha >/out; echo beta >>/out; cat </out'
```

The new `--process-tree` option enables scheduled nested processes, parent-specific
waits and reparenting orphaned children to the guest root. Four live children and
sixteen lifetime children/execs still bound the invocation. Earlier `--schedule`
mode retains the single-level process restriction.

Implemented support includes static PIE load bias with rejection of PT_INTERP
and DT_NEEDED dependencies, additional integer SIMD operations and register
loads/stores, synthetic UID/GID and uname, default/ignored signal dispositions,
signal masks and pending default termination. Static PIE startup performs its
own relocations in guest code. External library loading is still unsupported.
Unsupported sendfile returns ENOSYS so BusyBox cat can use read/write.

The script corpus covers variables, arithmetic, functions, for/while/case,
conditionals, printf, external execution, nested shells, pipelines, substitution,
redirection, stderr and exit status. Signal/process probes cover masked delivery,
ignored pending signals, inherited dispositions, nested waits and orphan reaping.
An independent assembly probe exercises SIMD output byte-for-byte. Malformed
ELF checks cover load bias, entry, dynamic table and external-loader rejection.

Eighty-nine local checks passed normally and under ASan/UBSan. Twenty-six extra
Linux comparisons passed using the exact pinned image and probe ELFs, including
all script cases, SIMD output, signal masks and process waits. Reports are
`mac-m24-validation.json`, `evidence/m24-sanitizer.json` and
`evidence/m24-linux-reference.json`. Source hashes identify each tested revision.

Excluded: caught signal handlers and traps, job control, chdir, unlimited trees,
general BusyBox applet compatibility, dynamic libraries and full filesystem
semantics. `rt_sigaction` returns EOPNOTSUPP for caught handlers; upstream ash can
ignore that error, so a script using `trap` must not be treated as supported.
M25 and later compatibility work must address these gaps. Production corpus
selection and independent confinement qualification remain open.

The OCI wrapper now owns terminal Ctrl-C forwarding and uses a dedicated host
notification signal to the interactive runtime. This avoids delivering a single
terminal Ctrl-C twice when both wrapper and child share the host foreground
group. Repeated PTY regressions verify that a child cancellation leaves the
guest parent alive.
