# Security policy

Dockiva Engine runs container code on the Mac's CPU and answers every Linux system
call itself, so the engine is the boundary between containers and the Mac. A
container that gets past it is a vulnerability, for example one that:

- reads or changes Mac files outside its root, volumes and bind mounts;
- changes an image's layers or another container's files;
- signals, traces or reads the memory of Mac processes or other containers;
- makes a published port listen on more Mac addresses than asked for.

The independent security review of this boundary is still open
([M13](docs/MILESTONES.md#security-review-m13)).

## Supported versions

Fixes go to `main`. Each Dockiva release bundles the engine commit it pins, so a
fix reaches users in the next Dockiva release. Older commits are not maintained.

## Reporting a vulnerability

Do not open a public issue. Use **Report a vulnerability** in this repository's
**Security** tab. Include the engine commit or Dockiva version, the macOS version
and Mac model, the impact, and steps to reproduce. If the form is not available,
follow [Dockiva's security policy](https://github.com/Tomnerb/Dockiva/blob/main/SECURITY.md).
