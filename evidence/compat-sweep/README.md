# Compatibility sweeps

Each case runs an image's typical workload on the VM-free engine (a test engine at
`~/.vmfree-clitest`) and checks the result: an HTTP answer, a query, or command output.

    python3 sweep1.py sweep1-results.json [case ...]   # 21 images
    python3 sweep2.py sweep2-results.json [case ...]   # 19 images and 3 Compose stacks

`sweep2.py` pulls, tests and removes one image set at a time (images listed in
`sweep3-keep.json` stay) and stops if less than 8 GiB of disk is free.

Results (2026-10-14): sweep 1 passes 21 of 21; sweep 2 passes 20 of 22.

- `minio`: neither Docker Hub nor quay.io serves the image anonymously any more (HTTP 401).
- `sonarqube`: everything starts, including its Elasticsearch 9 (seccomp, JVM self-attach);
  then Elasticsearch marks its indices read-only because the host disk was 98% full
  (flood-stage watermark), as it would under Docker on the same disk.
- `dotnet-sdk` checks `dotnet new`; `dotnet build` (MSBuild) still hangs.
