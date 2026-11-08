# dockivad

Go control plane for Dockiva Engine. No external Go modules
and no Python execution. Guest Linux semantics stay in the C runtime.

Build the complete signed helper on Apple Silicon from the repository root:

```sh
python3 scripts/bundle_dockiva_engine.py --output build/dockiva-go
build/dockiva-go/dockiva-engine/dockivad --home /tmp/dockivad-local
```

The Python command is build tooling. The resulting helper contains only Go/C
executables and prepared OCI fixture data. `dockiva-engine` is a separately signed
copy of `dockivad` for existing Dockiva helper discovery. Both run the same code.
Stop the API with SIGTERM; active container supervisors continue independently.
`POST /dockiva/shutdown` instead stops every running container as `docker stop`
does (its stop signal and grace period), answers 204 and exits; `always` and
`unless-stopped` containers start again with the next engine.
Use the authenticated Docker API subset or Dockiva for container lifecycle.

Images, containers, retained lowers and the build cache live on a case-sensitive
APFS volume (`store.go`): `<home>/store.sparsebundle`, attached at `<home>/store` with
`diskutil image attach` (never `hdiutil attach`, whose attachment makes every
barrier a full flush), because two Linux names that differ only by case are one
file on the Mac's case-insensitive APFS. Volumes, networks and sockets stay in
`<home>`. A home from before the store moves into it at the first start with no
container running; meanwhile the API answers only `/_ping`. `POST /dockiva/shutdown`
detaches the store and compacts the bundle, which keeps its largest size while
attached; any other exit detaches it when no container runs.

Import a bounded OCI layout while the daemon is stopped:

```sh
build/dockiva-go/dockiva-engine/dockivad --home /tmp/dockivad-local \
  --import-name local/demo:test --import-layout examples/m19/shell-image
```

`--pull registry/repository:tag` performs a verified scoped registry pull. OCI
linux/arm64 only: no links/devices, 32 layers, 32 MiB per blob, 128 MiB packed,
64 MiB expanded, 4096 members/paths. General stock-image support is not implied.
`--insecure-local` (for `--pull` and for the daemon's pull and push) lets
loopback registries (`127.0.0.1`, `localhost`) use plain HTTP; every other
registry, and loopback without the flag, is HTTPS only.

Tests (macOS needs external linking with the local Go 1.22 toolchain):

```sh
go -C dockivad test -race -ldflags=-linkmode=external ./...
python3 scripts/verify_dockivad.py \
  --helper build/dockiva-go/dockiva-engine/dockiva-engine \
  --report build/dockivad-recovery.json
```

State remains compatible with the legacy development tools. Import/pull and daemon
startup share an exclusive daemon lock. Requests on `engine.sock` (Dockiva's
endpoint) authenticate with the rotating private `docker-client/config.json`
token; `--tcp 0` explicitly selects a loopback test endpoint instead.

`docker.sock` serves the same API without the token, for the docker CLI and
Docker tools: like Docker's own socket, the private (0700) engine home and the
0600 socket are the access control. For the default home (`~/.dockiva-engine`, or `~/.vmfree` from before the rename) the engine
registers a `dockiva` Docker context, never switching the current one:

```sh
docker context use dockiva     # or: DOCKER_HOST=unix://$HOME/.dockiva-engine/docker.sock
docker run --rm redis:7-alpine sh -c 'echo hello'
```

Supported docker commands: `run` (foreground or `-d`, `-i`, `-t`, `--rm`,
`-p`, `-P`, `-e`, `-u`, `-m`, `--cpus`, `--entrypoint`, `-w`, `-v`, `--mount`,
`--read-only`, `--network`, `--network-alias`, `-h`, `--add-host`, `--restart`,
`--health-*`, `--stop-timeout`, `--init`), `exec` (`-i`, `-t`, `-u`, `-w`, `-e`, `-d`), `create`, `start`,
`stop`, `restart`, `kill` (TERM, INT, QUIT, HUP, USR1, USR2 reach the
container's first process; KILL ends it; a paused container takes KILL only, as
in Docker), `wait` (and the API's `next-exit` and `removed` conditions), `rm`
(with `-v`), `ps`, `inspect`, `logs` (with `-f`, `--tail`, `-t`, `--since` and
`--until`; stdout and stderr lines come in the order they were written, and
`--tail` counts both; logs written before this engine version carry their
file's last write time), `pause` and `unpause` (SIGSTOP and SIGCONT to the run's
and its execs' processes; healthchecks wait), `attach`, `rename` (stopped or
created containers; the ID stays), `update` (`-m`, `--cpus`, `--restart`; limits
apply from the next start, a policy from the run's next end), `events`
(container create, start, kill, die, stop, restart, pause, unpause, attach,
exec_create, exec_start, exec_die, health_status, rename, update, destroy; image
pull, tag, untag, delete, load, save; Docker's Actor attributes; `--since` and
`--until` over the latest 256; `type`, `container`, `event`, `image` and `label`
filters), `pull`, `push` (`--all-tags`), `images`, `tag`, `rmi`, `cp` (both
ways, including into volumes and bind mounts; read-only paths are refused),
`stats` (host CPU time, resident memory and disk I/O of the container's
processes, and its guest threads as PIDs; no network counters),
`volume` (`create`, `ls`, `inspect`, `rm`, `prune`), `network` (`create`, `ls`,
`inspect`, `rm`, `prune`, `connect`, `disconnect`), `docker compose` (`up`,
`down`, `ps`, `logs`, `exec`, `start`, `stop`, `restart`, healthchecks and
`depends_on` conditions), `system df` (`-v`), `save`, `load` (classic and OCI
archives, plain or gzip; linux/arm64 images only, others are skipped),
`container prune`, `image prune` (`-a`), `system prune` (`until`, `label`
and `label!` filters), `version`/`info`, and `build` and `compose build` (plain, through
buildx's `docker` driver, or with `DOCKER_BUILDKIT=0`). Builds run the
Dockerfile on the engine's classic builder: linux/arm64 only; results go to the
image store, and through buildx also to a directory or tar file
(`--output type=local,dest=DIR`, `--output type=tar,dest=FILE`: the result's
files, sent through the client's session). The context can be a local directory,
a Git repository (`https://...git#ref:subdir`, `git@host:repo`, `git://`,
`github.com/...`: cloned with the Mac's git, without `.git`) or an http(s) URL
(a tar archive, plain or gzip, bzip2, xz or zstd, or else the Dockerfile).
`--build-context name=VALUE` (buildx) takes a local directory, `docker-image://REF`,
or a Git or http(s) source; `FROM name`, `COPY --from=name` and
`RUN --mount=from=name` find it before stages and images. buildx's read of a
remote context's `Dockerfile.rego` goes through a minimal LLB evaluator (`llb.go`:
Git and HTTP sources and copies, with `StatFile`/`ReadFile`). `--call`/`--check`,
other `--output` types, and a build context that is a build target or an OCI
layout are refused.
tmpfs mounts (`docker run --tmpfs DIR[:size=,mode=,uid=,gid=,exec]`, `--mount
type=tmpfs,target=DIR[,tmpfs-size=,tmpfs-mode=]`) are file systems the runtime keeps
in memory, charged to the container's memory limit as Linux charges tmpfs, with
Docker's defaults (`noexec,nosuid,nodev`, mode 1777). They belong to the container's
own run: `docker exec`, healthchecks and `docker cp` see the empty mount point
beneath them.

Builds read the Dockerfile 1.x syntax (`# syntax=docker/dockerfile:1...` is
accepted; other frontends are refused, and `# check=` is ignored):

- `RUN --mount=type=cache,target=DIR[,id=,sharing=,from=,source=,mode=,ro]`: a
  directory kept across builds, keyed by `id` (the target by default), seeded
  from `from`/`source` when first made. Each one is a build cache entry of type
  `exec.cachemount`, so `docker buildx du`, `docker system df -v` and
  `docker builder prune` count and remove it (entries in use are kept).
  `sharing=locked` runs one step at a time per cache; `shared` and `private`
  both share it. `uid`/`gid` are accepted and ignored.
- `RUN --mount=type=bind[,source=,target=,from=,rw]`: a path of the build
  context, a stage or an image, read-only unless `rw` (a clone; changes are
  discarded). A context source's content is part of the step's cache key.
- `RUN --mount=type=tmpfs,target=DIR`: an empty directory, discarded after the step.
- `RUN --mount=type=secret,id=ID[,target=,env=,required,mode=]`: a secret from
  `docker build --secret id=ID,src=FILE` (buildx only; `DOCKER_BUILDKIT=0` has
  no secrets, so a `required` one fails), as a read-only file
  (`/run/secrets/ID` by default) and/or an environment variable. Secret values
  never enter the image or the cache key.
- `RUN --mount=type=ssh[,id=,target=,required,mode=]`: a socket forwarding to
  the client's SSH agent (`docker build --ssh default` or `--ssh ID=SOCKET`;
  buildx only), at `/run/buildkit/ssh_agent.N` unless `target`; the first one is
  the step's `SSH_AUTH_SOCK`. An optional agent the client lacks is skipped.
- `ADD` unpacks local tar archives, plain or gzip, bzip2, xz (through the Mac's
  bsdtar) or zstd; a compressed file that is not a tar is copied.
- `RUN --network=default|none|host`; `RUN --security=insecure` is refused.
- Here-documents: `RUN <<EOF` (the script, run by its `#!` line or the shell),
  `RUN python3 <<EOF` and several documents in one instruction (the shell does
  the redirecting), `<<-EOF` (leading tabs dropped), quoted delimiters
  (`<<'EOF'`: no expansion in `COPY`), and `COPY <<EOF /path` / `ADD` (an inline
  file, mode 0644 unless `--chmod`).

Mount points made for a step are removed from the stage's files afterwards.

Registries: `docker pull` and `docker push` use `docker login`'s credentials
(X-Registry-Auth: Basic, or Bearer tokens from the registry's token service,
including identity tokens). A push sends the stored manifest, config and layers
byte for byte (Docker v2 schema 2 or OCI, as the image has them), so pulling the
pushed tag gives the same image ID. Blobs the registry has are skipped ("Layer
already exists"); a blob another cached repository of the same registry holds is
mounted from it first ("Mounted from ..."); others upload (POST, PATCH, PUT).
`docker build --push` (buildx) pushes through the same API and reads the result
back with `GET /distribution/NAME/json`. Try it against a local registry:

```sh
dockiva-engine --insecure-local &
docker run -d -p 127.0.0.1:5055:5000 registry:2
docker tag alpine 127.0.0.1:5055/demo:1 && docker push 127.0.0.1:5055/demo:1
docker build --push -t 127.0.0.1:5055/app:1 .
```

Port 5000 is taken by macOS's AirPlay receiver; pick another. `FROM` in a build
pulls over HTTPS, or plain HTTP from a loopback registry with `--insecure-local`.

Networks: the default bridge and user networks give each container an address
in 10.89.N.0/24 and private ports (two containers can both listen on 5432, and
unpublished ports stay off the Mac). On a user network (Compose's default)
containers find each other by name and alias; `host.docker.internal` is the
Mac; `--network host` shares the Mac's network as before. One network per
container.

Durability: a container's `fsync`/`fdatasync` is an I/O barrier
(`F_BARRIERFSYNC`) by default, so writes reach the disk in order and a database
comes back consistent after power loss, losing at most its last commits (OrbStack
behaves the same). `dockiva-engine --fsync full` drains the drive's cache on every
fsync (strictly durable, about 5x slower for databases); `--fsync fast` is plain
macOS fsync (fastest; writes may reach the disk out of order).

Scratch files: `dockiva-engine --tmp memory` puts every container's `/tmp` (unless
something is mounted there) and `/dev/shm` (64 MiB) in such an in-memory tmpfs: no
macOS file system calls, so creating and removing files there costs about 2 us
instead of 50 us (an APFS create). It is off by default (`--tmp disk`): Docker keeps
`/tmp` on the container's writable layer (it survives a restart and is visible to
`docker exec`), and its contents count against the memory limit.

Published ports: `-p 8080:80` listens on 127.0.0.1 only, so nothing is reachable
from the network by default; with `--default-bind-ip 0.0.0.0` it listens on every
address of the Mac (0.0.0.0 and ::, as Docker does). `-p 127.0.0.1:8080:80`, `-p 192.168.1.5:8080:80` and
`-p [::]:8080:80` bind that address only. Port ranges (`-p 8000-8005:8000-8005`),
`-p 80` and `-P` (free host ports for every exposed port) and UDP (`-p 53:53/udp`)
work. The container binds a private loopback port and its supervisor forwards
each published address to it (TCP with half-closes, UDP with a session per
client that ends after 90 idle seconds); a single TCP port on 127.0.0.1 is bound
by the container directly. A port already in use fails the start, as in Docker.
macOS may ask once whether `dockiva-engine` may accept incoming connections. Restart policies and healthchecks are run by each container's
supervisor; `always` and `unless-stopped` containers start again with the
engine.

`-t` gives the container (or exec) a pseudo-terminal: line editing, ^C to the
foreground job and resizes work. `docker exec` runs a second runtime on the
container's root, mounts and network, so it shares files, volumes, localhost
ports and Unix sockets (`psql`, `redis-cli` and shells work) but cannot see or
signal the container's own processes (`ps`, `kill` inside an exec).

Mounts: `-v /host/path:/path[:ro]` binds a host directory or file (a missing
host directory is created, as Docker does); `-v name:/path` and `-v /path` use
named and anonymous volumes in `<home>/volumes/<name>/_data`. A new volume first
receives what the image has at its mount point (unless `nocopy`). Mount points
are created in the container root, resolving its symlinks; the runtime serves
them with `--bind`/`--bind-ro`, keeping symlinks and `..` inside the container.

Resources: `-m` caps a container's guest memory (6 MiB to 64 GiB; otherwise
its profile's limit, 512 MiB) and is what `free`, `/proc/meminfo` and sysinfo
report; a process that exceeds it is killed. `--cpus N` sets the vCPU count,
rounded up (default: one per host CPU), not a CFS quota. At most 64 containers
run at once (`dockiva-engine --max-containers`). Limits are caps, not reservations. The C guard applies core=0,
file size=16 MiB, descriptors=128 and CPU=60 seconds to workers. Cross-store/JIT
accounting, pressure feedback, production confinement and broader workload
compatibility remain separate architecture gates.

Execution profiles are defined in the embedded `profiles.json` catalog. New
containers persist a digest-bound policy; Create/Start and supervisor launch
query native `capabilities` before guest execution. Inspect includes
`DockivaCompatibility`. Legacy state retains its existing configuration. See
[profile semantics and verification](../docs/DOCKIVA-INTEGRATION.md#compatibility-profiles).
The test suite now requires both built native executables for real capability probes.
