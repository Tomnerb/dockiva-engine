# M30: differential compatibility harness

`python3 scripts/verify_m30.py --linux-reference --report mac-m30-validation.json`
replays the versioned corpus against the interpreter and pinned ARM64 Alpine.
Each backend gets a fresh filesystem. Reports include runtime/source/corpus hashes,
actual exit status/stdout/stderr, the reference image identity and classification.

Eight cases currently match Linux. Two filesystem differences are explicitly
recorded: physical parent traversal and symlink following remain denied. Five
saved malformed/resource cases verify bounded rejection. Every unexpected runtime
or reference outcome fails the run; declaring a policy gap does not suppress
changes to either expected outcome. Four unit tests exercise the classifier.
Runs without Linux are labeled local-only and are not differential evidence.

The malformed corpus pins source hashes and exact mutation recipes. The workflow
in `.github/workflows/compatibility.yml` builds with sanitizers and replays that
corpus on Linux. Its configuration is present; no hosted CI execution has been
observed in this local workspace. Local and sanitizer evidence is recorded
separately. Larger application, concurrency and fuzz corpora remain future work.
