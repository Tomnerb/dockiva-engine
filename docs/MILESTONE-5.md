# Milestone 5: a Linux HTTP server over real TCP

The compiled C interpreter now adapts a small set of Linux ARM64 networking
syscalls to host sockets. An independently compiled Linux ELF server handles a
real HTTP request from a host client. The HTTP parsing and response are guest C
code; the runtime only interprets instructions and adapts system calls.

## Verify on your Mac

Extract into a fresh directory. Requires Python 3.10+ and Xcode Command Line Tools
(or another local C compiler). The Linux ELF fixtures are included.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m5.py --report mac-m5-validation.json
python3 scripts/verify_m4.py
python3 scripts/verify_m3.py
python3 scripts/verify_m2.py
```

Expect 14 M5 checks, 17 M4 checks, 22 M3 checks and 20 M2 checks to pass. The M5
verifier starts and cleans up its own one-request servers on ephemeral ports.
`Linux reference: not-run` is expected on the Mac command above.

## Try with curl

Prepare two terminals in the extracted project. In terminal 1:

```sh
./build/vmfree-c run examples/m5/server.elf
```

It prints `PORT 18080`. Within 30 seconds, run in terminal 2:

```sh
curl --noproxy '*' --max-time 10 http://127.0.0.1:18080/
```

Expected body:

```text
VM-free HTTP server OK
```

The guest server exits with status 0 after one completed response. Start it again
for another request. The guest requests SO_REUSEADDR so the port can be reused.
If 18080 is already in use, start with `-- auto` to request an ephemeral port and
use the printed port in curl instead:

```sh
./build/vmfree-c run examples/m5/server.elf -- auto
```

Any extra guest argument currently selects an ephemeral port; this is not a port
number parser. A browser can make extra requests, so curl is the simpler client
for this deliberately one-connection demo. Ctrl+C stops a waiting manual run.

## Protocol scope

The demo responds to GET / with 200, other GET paths with 404, and non-GET methods
with 405. It reads fragmented headers until CRLF CRLF, up to 2048 bytes, and sends
431 when that limit is exhausted. Responses contain Content-Length and close the
connection. A disconnect before complete headers returns guest exit code 4.
It is not a general HTTP parser: no request-body handling, keep-alive, pipelining,
TLS, HTTP/2, concurrency or production robustness is claimed.

## Syscall scope

| Linux ARM64 call | Number | Supported subset |
|---|---:|---|
| socket | 198 | AF_INET, SOCK_STREAM, protocol 0 or TCP; no socket flags |
| bind | 200 | Linux 16-byte sockaddr_in, exactly 127.0.0.1 |
| listen | 201 | Explicitly loopback-bound sockets, backlog 0..128 |
| accept | 202 | NULL peer address and length outputs |
| getsockname | 204 | Marshal Linux sockaddr_in and 32-bit socklen, including truncated buffers |
| setsockopt | 208 | Linux SOL_SOCKET/SO_REUSEADDR, 32-bit integer only |
| read | 63 | Managed sockets into writable guest memory |
| write | 64 | Managed sockets plus the existing stdout/stderr path |
| close | 57 | Managed socket descriptors only |

Linux socket numbers and sockaddr bytes are decoded explicitly. macOS's socket
address layout and option numbers are not assumed to match Linux. Common host
errno values are translated to Linux values; unknown errors fall back to EIO.

## Runtime policies and limits

- Separate guest descriptor table, 32 sockets maximum; host descriptor numbers
  are never returned as guest descriptors. Socket cleanup occurs on normal exit
  or runtime errors; OS process cleanup handles termination.
- Localhost binding only; wildcard and other addresses are denied. listen on an
  unbound socket is denied to prevent implicit wildcard binding. These are scope
  limits, not evidence of a complete security sandbox.
- Blocking sockets. Adapter timeouts: 30 seconds waiting for accept and 5 seconds
  for accepted-socket I/O, implemented through host socket timeout options. These
  are prototype policy; they differ from Linux defaults. The verifier separately
  enforces process/client timeouts and cleans up failed runs.
- Per-call socket I/O capped at 1 MiB. Guest buffers are validated in full before
  host I/O; Linux partial-fault behavior is not fully reproduced.
- SIGPIPE is ignored so socket write failures return errors instead of killing
  the runtime; guest Linux signal delivery is not implemented.
- No connect, DNS, UDP, IPv6, nonblocking I/O, poll/epoll, accept4, arbitrary socket
  options, stdin reads, file descriptors for disk files, or shutdown syscall.
- The instruction budget does not measure wall time spent in a blocking syscall.

## Validation

Six end-to-end server scenarios: root, missing route, method rejection,
fragmented headers, oversized headers, and early disconnect. Eight additional
probes cover descriptor errors/lifetime, bad pointers, localhost policy,
unsupported UDP, descriptor capacity and implicit-bind rejection.

Nine checks (the six server scenarios plus three unambiguous descriptor/pointer
checks) match QEMU user-mode on Linux using the same ELF programs. Five checks are
explicit runtime-policy checks. In particular, simultaneous invalid descriptor
and invalid pointer error ordering is not asserted to match every Linux reference.
M5 Mac execution and native ARM64 Linux hardware comparison remain pending.

```sh
# Optional reference on Linux:
python3 scripts/verify_m5.py --qemu /path/to/qemu-aarch64 --report linux-m5.json

# Reference on ARM64 Linux hardware:
chmod +x examples/m5/*.elf
python3 scripts/verify_m5.py --native-linux --report arm64-m5.json

# Rebuild fixtures with Zig 0.13.0/LLVM:
python3 scripts/build_m5.py --zig /absolute/path/to/zig
```

The fixture build is freestanding O0 C with integer registers only, no libc,
compiler runtime checks or stack-protector helpers. Source and ELF hashes are
recorded. The active networking adapter is native/net.h. This is still a small
compatibility experiment, not arbitrary Linux or Docker image support.

References:
- [Linux ARM64 generic syscall definitions](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)
- [Apple socket documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/socket.2.html)
