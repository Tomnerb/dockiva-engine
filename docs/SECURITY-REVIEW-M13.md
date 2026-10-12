# M13 security review and open acceptance gate

This is an implementation review of the prototype, not an independent security
audit or approval to run hostile workloads. The production-isolation acceptance
gate remains OPEN.

## Intended boundary

Trusted, supported Linux ARM64 guest ELFs are interpreted as data. Guest syscall
numbers are dispatched explicitly, never passed through to the host kernel.
A container has a private rootfs copy and a bounded guest descriptor table.
The manager and image cache belong to one local OS user. Other processes running
as that user, root, concurrent host mutation and compromised host libraries are
outside the present boundary.

Network policy is host loopback IPv4 TCP only, for both bind and connect. This
allows service communication but also lets a guest contact other local services.
It is NOT per-container network isolation or an allowlist of destination ports.
No registry image is executed merely because it was downloaded.

## Changes and reviewed findings

| Area | Result |
|---|---|
| Registry import | Fixed a potential unreferenced-link copy: import now copies only verified manifest/config/layer blobs. A regression fixture includes a link to an unrelated host file. |
| Registry transport | HTTPS verification uses the platform trust store. Plain HTTP requires explicit opt-in and literal 127.0.0.1. Redirects fail instead of forwarding credentials. |
| Registry authentication | Explicit Bearer token or anonymous HTTPS Bearer challenge. Tokens stay out of cache metadata and diagnostics. Empty, oversized and newline-bearing tokens are rejected. |
| Image integrity | Requested manifest digest, descriptor sizes/hashes and uncompressed DiffIDs are checked before commit. Digest integrity does not authenticate a publisher. |
| Engine control | Random token, private client config, loopback TCP or private Unix socket, no CORS, bounded requests and 16 request workers. Unauthorized requests fail. |
| Filesystem | Root-relative directory descriptors, no-follow traversal, private copies, write budgets and earlier path/link rejection tests. Guest links/devices/mounts are unsupported. |
| Process authority | Supervisor-owned child lifecycle; a PID in metadata is not sufficient authority to stop a process. Running-instance locks and stop tokens remain enforced. |
| Host resource limits | Managed child: no core dumps, 16 MiB maximum file size (also caps each log file), 128 descriptors, 60 CPU seconds. Existing guest instruction/memory limits remain. |
| Unsupported API settings | Privileged mode, host mounts, custom security settings, port remapping and unsupported platforms fail. |

## Remaining risks and work

- The C interpreter and adapters are trusted native code. A memory safety defect
  could expose the host process. Sanitizer success is not proof of isolation.
- No verified macOS Seatbelt/App Sandbox boundary or Linux seccomp policy exists.
  Resource limits are not a filesystem/network sandbox.
- No aggregate persistent-disk quota, container CPU fairness or guest memory
  accounting across all containers exists. Restart resets per-process budgets.
- Host-created hard links, same-user tampering and concurrent host rootfs changes
  are not protected by a complete capability model. Only loader-created private
  trees are supported. Mac case-insensitive filesystem semantics also differ.
- Cached names are immutable until managed manually; update/eviction policy,
  publisher signatures, credential helpers and registry redirect/CDN support
  remain open. Bearer token servers are trusted as directed by the chosen registry.
- The HTTP demo has eight client slots and no per-client idle deadline. Slow
  clients cannot block unrelated reads within available slots, but can occupy
  all slots. Its upstream relay uses bounded blocking I/O, not a fully asynchronous
  proxy. This is not a public-facing production HTTP implementation.
- Abrupt supervisor failure can leave a running process with stale metadata.
  Automatic recovery, stronger process identity and transactional crash repair
  still need design and adversarial tests.
- Registry downloads use HTTPS in production code, but this release's automated
  registry fixture is local HTTP. Public authenticated/CDN registries are not an
  end-to-end acceptance result.

## Evidence and required next review

The combined suite exercises malformed/unsupported control requests, unauthenticated
access, corrupted registry blobs, link imports, networking/error paths and existing
filesystem/OCI rejection cases. Native paths pass AddressSanitizer and
UndefinedBehaviorSanitizer with leak detection disabled by environment limitation.

Before untrusted-image support: commission an independent review, define a tested
OS confinement policy, add mutation/race and parser fuzz campaigns, close aggregate
resource gaps, and test denial-of-service and crash recovery on the target Mac.
