# Combined M11–M17 release candidate

This release implements the networking, image-distribution, Docker API and
Compose subsets below, plus hardening, benchmarking and Mac packaging tooling.
It does NOT close the independent security review, same-Mac VM/resource comparison,
or signed clean-Mac distribution gates.

| Milestone | Delivered | Acceptance status |
|---|---|---|
| M11 | Nonblocking sockets, ppoll, accept4, loopback connect/shutdown, persistent HTTP demo | Linux checks pass; Mac pending |
| M12 | Verified OCI pull/cache, ARM64 index selection, import/inspect/list | Local Distribution API checks pass; public HTTPS/auth/CDN acceptance pending |
| M13 | Resource limits, authenticated control, rejection tests, implementation review | Production isolation review OPEN |
| M14 | Comparable HTTP workload and timing harness; Linux-only measurements | Same-Mac VM comparison and CPU/RSS accounting OPEN |
| M15 | Authenticated Engine API subset | Docker CLI 27.5.1 workflows pass on Linux; Mac pending |
| M16 | JSON Compose subset runner plus actual Docker Compose host-network example | Docker Compose 2.32.4 workflows pass on Linux; Mac pending |
| M17 | Mac packaging/signing/notary scripts, installer and diagnostics | Signed installation, zero-dependency packaging, updates/rollback acceptance OPEN |

## Mac acceptance

Extract the full source package into a fresh directory. Keep the Mac feature-macro
fix from M7–M10; it is already included.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m11_17.py --report mac-m11-17-validation.json
python3 scripts/verify_m7_10.py
```

Expect 30 new PASS lines and 40 from M7–M10. The new suite uses temporary state,
local HTTP registry fixtures and a token-authenticated loopback API. It needs no
Docker Desktop, registry account or Linux toolchain. It removes its own instances.

Optional checks against locally installed client executables:

```sh
python3 scripts/verify_m11_17.py \
  --docker /path/to/docker \
  --compose /path/to/docker-compose \
  --report mac-m11-17-clients.json
```

Docker CLI adds nine checks and Docker Compose adds six. The Compose executable
must be the standalone v2 binary (the same binary can serve as a CLI plugin).
The Linux-only `--qemu /path/to/qemu-aarch64-static` adds one network ABI check.
All options together yielded 46 PASS lines here. Linux sanitizer validation covers
the 30 default checks; leak detection was disabled due to host inspection limits.

## Start the local engine and run two services

First import the bundled supported image:

```sh
python3 vmfree.py image import vmfree/server:demo examples/m11_17/http-image
python3 vmfree.py engine
```

The Mac engine defaults to a private Unix socket. If Unix sockets are unavailable,
use `python3 vmfree.py engine --tcp 0`; this still binds only to 127.0.0.1 and
requires the same token. Run it in its own terminal. Ctrl-C stops the API server;
running containers retain their supervisors. Stop workloads before shutting down
if you want all guest processes to exit.

In a second terminal:

```sh
python3 vmfree.py compose -f examples/m11_17/compose.json up
curl --noproxy '*' --max-time 10 http://127.0.0.1:18081/
python3 vmfree.py compose -f examples/m11_17/compose.json ps
python3 vmfree.py compose -f examples/m11_17/compose.json stop
python3 vmfree.py compose -f examples/m11_17/compose.json up
python3 vmfree.py compose -f examples/m11_17/compose.json down
```

The frontend on 18081 connects to the backend on 18082 and relays its HTML.
The response includes `Persistent VM-free HTTP server OK`. The backend increments
its private `/counter` on startup. Stop/up retains that file; down deletes the
containers and their private data. Named volumes are NOT implemented. Choose other
explicit ports in the example if these are occupied; ports are shared with the host.

The bundled runner accepts JSON syntax (a valid YAML subset), exec-form command
arrays, string environment maps, read_only, depends_on lists, image names and
explicit network_mode: host. It rejects other Compose features before making
changes. Dependencies control start order, not readiness. `up` is idempotent for
unchanged services; changed configs require explicit down/recreation. Failed starts
trigger best-effort cleanup of newly created instances.

## Use the real Docker CLI

The wrapper targets this engine with its private authentication config and API
version 1.45. It does not change your default Docker context.

```sh
python3 vmfree.py docker version
python3 vmfree.py docker images
python3 vmfree.py docker create --network host --name web vmfree/server:demo 18080
python3 vmfree.py docker start web
python3 vmfree.py docker ps -a
python3 vmfree.py docker inspect web
python3 vmfree.py docker logs web
python3 vmfree.py docker stop web
python3 vmfree.py docker rm web
```

These commands require the Docker CLI, but not Docker Desktop. Tested here with
Docker CLI 27.5.1. Other versions remain acceptance targets, not assumed compatible.
If your CLI discovers its Compose plugin:

```sh
python3 vmfree.py docker compose -f examples/m11_17/compose.json up -d
python3 vmfree.py docker compose -f examples/m11_17/compose.json ps -a
python3 vmfree.py docker compose -f examples/m11_17/compose.json stop
python3 vmfree.py docker compose -f examples/m11_17/compose.json down
```

The real Compose 2.32.4 binary passed up/ps, service traffic, stop, up again and
down against this API on Linux. That does not imply general Compose compatibility.
Custom Docker client config directories can affect plugin discovery; the bundled
`vmfree.py compose` runner is available without installing a plugin.

Supported API families: ping/version/info; cached image list/inspect and scoped
pull; container create/list/inspect/start/stop/restart/remove; non-streaming logs;
not-running wait with a 30-second cap. The advertised API range is 1.24–1.45 for
these endpoints only. Unsupported endpoints/options return errors, including
attach/exec/build, TTY/stdin, streaming log follow, namespaces, volumes/mounts,
bridge/DNS networking, port publishing/remapping, privileged execution, custom
resource settings and arbitrary Linux images. `docker run -d --network host`
may use these basic endpoints; the acceptance suite uses explicit create/start.

## Registry pulls

```sh
python3 vmfree.py image pull registry.example.com/team/supported-image:tag
python3 vmfree.py image inspect registry.example.com/team/supported-image:tag
```

Replace the example with a registry containing a compatible image. This release
has not published your image to an external registry. Use an explicit registry
hostname; Docker Hub shorthand is not expanded. HTTPS is required. For an explicit
local test registry only, `--insecure-local` permits literal 127.0.0.1 HTTP.

Supported: OCI manifests/configs, one-level OCI indexes selecting exactly one
linux/arm64 manifest, tar/gzip layers, SHA-256 descriptor and DiffID verification,
bounded downloads, anonymous Bearer challenges over HTTPS, or an explicit token
read from a named environment variable via `--token-env VARIABLE_NAME`.
No credentials are copied into the cache. Redirects/CDNs, Basic auth, credential
helpers, resumable downloads, Docker schema media types and publisher signatures
are not supported. Normal large distro images often exceed the loader's feature
and size limits. Pull validates compatibility metadata; it does not execute a guest.

Each cache name is immutable: re-pulling the same digest is allowed, but changing
an existing name to another digest fails. Use a new tag/digest reference instead.
There is no general tag-update/eviction/delete CLI yet. Cache entries contain only
verified referenced blobs; unreferenced source files and links are not imported.

## Networking and resource scope

M11 supports IPv4 TCP SOCK_NONBLOCK/CLOEXEC, fcntl GETFL/SETFL for sockets,
accept4 with null peer outputs, ppoll over up to 32 guest socket entries, connect
to 127.0.0.1 and shutdown. ppoll has a maximum 30-second wait and rejects signal
masks. No UDP, external networking, epoll, DNS or guest signal implementation.

The demo multiplexes eight clients, handles fragmented requests and partial normal
response writes, and closes each HTTP connection. It accepts GET / only; unsupported
requests get a minimal 400 response. Error responses are best effort. The upstream
relay uses bounded blocking calls; this is not a production proxy or HTTP stack.
Managed execution retains the 10-million-instruction budget, so persistent here
means repeated requests within that budget, not an unlimited server lifetime.

Host limits added to managed guests: 60 CPU seconds, no core dumps, 128 host file
descriptors and 16 MiB per host file (including each output log). These supplement
existing guest memory/file/write budgets; they do not establish production isolation.

## Remaining gates and reproducibility

- [M13 review and threat model](SECURITY-REVIEW-M13.md)
- [M14 benchmark setup and limitations](BENCHMARK-M14.md)
- [M17 Mac packaging and open distribution requirements](MAC-DISTRIBUTION-M17.md)

`python3 scripts/build_m11_17.py --zig /path/to/zig` rebuilds the guest fixtures
with Zig 0.13.0. Original C/assembly, hashes and command flags are included.
The downloaded Docker and Compose validation binaries are not bundled.

References used for interoperability:

- [Docker Engine API 1.45](https://docs.docker.com/reference/api/engine/version/v1.45/)
- [Docker client headers/config](https://docs.docker.com/reference/cli/docker/#custom-http-headers)
- [Compose specification](https://docs.docker.com/reference/compose-file/)
- [OCI Distribution specification](https://github.com/opencontainers/distribution-spec/blob/main/spec.md)
