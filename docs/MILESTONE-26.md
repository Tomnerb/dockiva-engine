# M26: scheduled networking foundation

`--scheduled-network` enables cooperative loopback socket waits in the native
runtime, OCI runner and managed-container configuration. A blocking accept,
connect, read, write or socket poll suspends only its guest task. The scheduler
checks readiness and monotonic deadlines while CPU and pipe tasks continue.
Guest nonblocking flags are independent of the permanently nonblocking host
socket. Signals cancel blocked guests and cleanup releases their sockets.

Validation: `mac-m26-validation.json` records 29 checks including cache variants,
OCI execution, managed restart/log retention/removal, and five ARM64 Linux
comparisons. `evidence/m26-sanitizer.json` covers 19 command/OCI checks.
`evidence/m26-native.txt` records five sanitizer probes that additionally verify
actual suspension and descriptor/task/pipe cleanup. The existing M11–M17 suite
passed 30 checks with scheduled networking enabled, including concurrent HTTP,
slow peers, disconnects, lifecycle, registry, Docker API and Compose subsets
(`evidence/m26-scheduled-http-regression.json`).

Scope remains IPv4 TCP loopback. Fork/exec with live sockets, mixed pipe/socket
polling, DNS, outbound access and general Docker networks remain unsupported.
Timeouts are bounded to 30 seconds; a zero socket timeout uses that upper bound.
This is a scheduler foundation, not production network compatibility.
