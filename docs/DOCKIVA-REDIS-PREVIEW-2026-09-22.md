# Local Dockiva Redis test — 2026-09-22

The prepared Redis 7.4.2 fixture is now packaged with the experimental Unicorn
adapter in a separate local Apple Silicon Dockiva test application.

Open `/Users/macbookpro16m2max/GolandProjects/devstack/devstack/bin/Launch Dockiva Redis Test.command`.
This launches `dockiva-redis-test.app` using `~/.vmfree-dockiva-redis-test` for
engine state. Existing app artifacts and engine stores remain separate.
Select VM-free, open Create container, choose **Redis preview**, and create it.
Leave Command and port mappings empty. The bundled engine starts automatically
when VM-free is selected; no Python, Homebrew or terminal setup is required.
Connect a Redis client to `127.0.0.1:16379` after Redis finishes loading.
Only one server can use that port at a time.

The digest-pinned `vmfree/redis:preview` image alone selects Unicorn, dynamic
loading and bounded larger ELF/mmap limits. Other image profiles retain the
interpreter. Redis uses its own writable root and `/data` with AOF persistence
and `appendfsync always`. Stop/Start and Restart keep data; Remove deletes it.
Dockiva Stop currently terminates the runtime; the persistence test verifies
recovery of acknowledged writes after this operation, not guest SHUTDOWN semantics.
Execution limits still apply, including 60 host CPU seconds and a billion guest
instructions per run. This is a bounded development preview, not a durable service
deployment. Background fork/AOF rewrite remains unsupported.

Validation against the final signed app's helper passed: automatic engine startup
with developer tools absent from PATH; shell and HTTP regressions; Redis
PING/SET/GET; listener closure after Stop; and saved-value recovery after Start
and Restart. Readiness checks wait through Redis's LOADING response.
Four Go VM-free tests passed; the separate live-engine test was skipped because
no external engine was supplied. 59 Python regression tests and four new profile
guard tests passed. The frontend and production Go application built successfully.
Ad-hoc deep signature verification passed, with the Unicorn JIT entitlement
restored after bundle signing. Go tests emitted existing SDK minimum-version
linker warnings; older macOS versions were not tested.

The application was launched and its process confirmed. Native GUI clicks were
not automated. Stock Redis images, PostgreSQL, Valkey, production isolation,
distribution/licensing, RAM/CPU/battery savings and actual 8 GB hardware remain
outside this result. No milestone or production acceptance is implied.

Evidence and artifact hashes: [report](../evidence/dockiva-redis/report.json),
[packaged adapter tests](../evidence/dockiva-redis/adapter-tests.txt),
[Python regressions](../evidence/dockiva-redis/python-tests.txt),
[profile guards](../evidence/dockiva-redis/profile-tests.txt).

To rebuild the helper, first prepare the pinned database fixture using the
existing database-fixture workflow, then run `scripts/bundle_dockiva_engine.py`
from an Apple Silicon Python environment with PyInstaller 6.22.3. The builder
verifies fixture hashes and creates the prepared OCI image. Dockiva's macOS
packaging task preserves the nested Unicorn JIT entitlement.
