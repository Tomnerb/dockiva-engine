# GitHub Actions on the VM-free engine

Two ways of running GitHub Actions workflows, checked against the VM-free engine
(arm64, `DOCKER_HOST=unix://$HOME/.vmfree-clitest/docker.sock`).

## 1. act (nektos/act 0.2.89): workflows run locally

```
act push -P ubuntu-latest=node:22 --pull=false \
    --container-daemon-socket - --container-architecture linux/arm64
```

[`ci.yml`](ci.yml) has two jobs, run in parallel:

- `build` (`node:22`, host network): `actions/checkout@v4`, a shell step reading
  the checked-out README, step outputs through `$GITHUB_OUTPUT`, node, files in the
  workspace.
- `container-job` (`python:3.12-alpine` job container with a `redis:7-alpine`
  service on a user network): python, and a PING to the service by name.

Result: both jobs succeed, three runs out of three ([`act-run.log`](act-run.log)).

Engine changes this needed:

- Container and network names as Docker accepts them (act's are long and dotted);
  the hostname falls back to the short ID when the name is not a valid host name.
- `docker cp` into a container (PUT /containers/{id}/archive) creates parent
  directories a tar leaves out, and resolves mounts per entry: act extracts at
  `/` into the workspace and env volumes.

## 2. Self-hosted runner image (ghcr.io/actions/actions-runner, 2.338.0)

- `./bin/Runner.Listener --version`: 2.338.0 (.NET 8).
- `./config.sh --help`: prints its help (the libicu check passes).
- `./run.sh` unconfigured: "Not configured. Run config.(sh/cmd)", as on Linux.
- `./config.sh --unattended --url https://github.com/Tomnerb/Dockiva --token <invalid>`:
  reaches `POST https://api.github.com/actions/runner-registration` over HTTPS
  and gets GitHub's 404 for the invalid token. Registering needs a real token.

Engine changes this needed:

- A relative command (`./config.sh`) resolves from the working directory, for
  `docker run` (image WORKDIR or `--workdir`), `docker exec` and healthchecks.
- `splice` and `tee` return EINVAL, so GNU grep (output to /dev/null) falls back
  to read and write instead of failing with ENOSYS.
- `clone3` (glibc falls back to clone) is no longer reported as missing.
