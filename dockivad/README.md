# dockivad

Go control plane for the supported VM-free preview engine. No external Go modules
and no Python execution. Guest Linux semantics stay in the C runtime.

Build the complete signed helper on Apple Silicon from the repository root:

```sh
python3 scripts/bundle_dockiva_engine.py --output build/dockiva-go
build/dockiva-go/vmfree-engine/dockivad --home /tmp/dockivad-local
```

The Python command is build tooling. The resulting helper contains only Go/C
executables and prepared OCI fixture data. `vmfree-engine` is a separately signed
copy of `dockivad` for existing Dockiva helper discovery. Both run the same code.
Stop the API with SIGTERM; active container supervisors continue independently.
Use the authenticated Docker API subset or Dockiva for container lifecycle.

Import a bounded OCI layout while the daemon is stopped:

```sh
build/dockiva-go/vmfree-engine/dockivad --home /tmp/dockivad-local \
  --import-name local/demo:test --import-layout examples/m19/shell-image
```

`--pull registry/repository:tag` performs a verified scoped registry pull. OCI
linux/arm64 only: no links/devices, 32 layers, 32 MiB per blob, 128 MiB packed,
64 MiB expanded, 4096 members/paths. General stock-image support is not implied.
`--insecure-local` is restricted to literal 127.0.0.1 for registry tests.

Tests (macOS needs external linking with the local Go 1.22 toolchain):

```sh
go -C dockivad test -race -ldflags=-linkmode=external ./...
python3 scripts/verify_dockivad.py \
  --helper build/dockiva-go/vmfree-engine/vmfree-engine \
  --report build/dockivad-recovery.json
```

State remains compatible with the legacy development tools. Import/pull and daemon
startup share an exclusive daemon lock. Requests on `engine.sock` (Dockiva's
endpoint) authenticate with the rotating private `docker-client/config.json`
token; `--tcp 0` explicitly selects a loopback test endpoint instead.

`docker.sock` serves the same API without the token, for the docker CLI and
Docker tools: like Docker's own socket, the private (0700) engine home and the
0600 socket are the access control. For the default home (`~/.vmfree`) the engine
registers a `dockiva` Docker context, never switching the current one:

```sh
docker context use dockiva     # or: DOCKER_HOST=unix://$HOME/.vmfree/docker.sock
docker run --rm redis:7-alpine sh -c 'echo hello'
```

Supported docker commands: `run` (foreground or `-d`, `--rm`, `-p`, `-e`,
`--entrypoint`, `-w`, `-v`, `--mount`, `--read-only`), `create`, `start`,
`stop`, `restart`, `kill` (TERM, INT, QUIT, HUP, USR1, USR2 reach the
container's first process; KILL ends it), `wait`, `rm` (with `-v`), `ps`,
`inspect`, `logs` (with `-f` and `--tail`), `attach --no-stdin`, `events`
(create, start, die, destroy), `pull`, `images`, `volume` (`create`, `ls`,
`inspect`, `rm`, `prune`) and `version`/`info`. Not supported: `-i`/`-t` and
`exec`, builds, tmpfs mounts, networks, `cp`, `stats`.

Mounts: `-v /host/path:/path[:ro]` binds a host directory or file (a missing
host directory is created, as Docker does); `-v name:/path` and `-v /path` use
named and anonymous volumes in `<home>/volumes/<name>/_data`. A new volume first
receives what the image has at its mount point (unless `nocopy`). Mount points
are created in the container root, resolving its symlinks; the runtime serves
them with `--bind`/`--bind-ro`, keeping symlinks and `..` inside the container.

Current resource policy is four live runtimes and 1024 MiB reserved guest memory
per state store, not a machine-wide or RSS cap. The C guard applies core=0,
file size=16 MiB, descriptors=128 and CPU=60 seconds to workers. Cross-store/JIT
accounting, pressure feedback, production confinement and broader workload
compatibility remain separate architecture gates.

Execution profiles are defined in the embedded `profiles.json` catalog. New
containers persist a digest-bound policy; Create/Start and supervisor launch
query native `capabilities` before guest execution. Inspect includes
`DockivaCompatibility`. Legacy state retains its existing configuration. See
[profile semantics and verification](../docs/DOCKIVA-INTEGRATION.md#compatibility-profiles).
The test suite now requires both built native executables for real capability probes.
