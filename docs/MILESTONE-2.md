# Milestone 2: compiled interpreter experiment

This package adds a portable C11 implementation of the existing interpreter.
The host executable is native code; guest ARM64 instructions are still interpreted.
No JIT, Linux VM, or Docker daemon is involved. C is the experiment's language,
not a final commitment for the production execution core.

## Run on your Mac

Extract into a fresh directory. Requires Python 3.10+ and a local C compiler
(Xcode Command Line Tools provides `cc`). The package contains source, not a
prebuilt or signed Mac executable.

```sh
cd vmfree-runtime
python3 scripts/build_native.py
python3 scripts/verify_m2.py --report mac-m2-validation.json
python3 scripts/benchmark_m2.py --report mac-m2-benchmark.json
```

Expect 20 PASS lines from verification and two benchmark summary lines. If your
Mac reports that developer tools are missing, install Xcode Command Line Tools
and rerun the build. The build uses your Mac's default architecture.

To run one program manually:

```sh
./build/vmfree-c run examples/independent/stdout.elf --trace
```

The original Python tests remain available:

```sh
python3 -m unittest discover -s tests -v
```

## Verification

The verifier first checks source/binary hashes and the Python M1 results. It then
compares the C backend against all six expected stdout/stderr/status triples.
Fourteen additional cases compare bounded rejection with the Python reference:
truncation, wrong architecture, overflowed program-header offset, excessive guest
memory, nonexecutable entry, invalid instruction, unsupported syscall, infinite
branches and reserved instruction encoding. Diagnostic wording may differ.

The ELF loader decodes bytes explicitly rather than casting untrusted bytes to
host structures. Bounds are checked with subtraction to avoid unsigned overflow.
Guest memory remains interpreter-owned and never becomes a host execution pointer.

This is still not an untrusted-code sandbox. System-call coverage, host-error
translation, instruction support and resource limits remain deliberately small.

## Benchmarks and interpretation

- `startup_exit`: launch runtime, load ELF, interpret exit_group, terminate.
- `branch_dispatch`: launch runtime, execute 500,000 branch instructions, hit the
  instruction budget and terminate with the expected error. This is a synthetic
  dispatch test, not a complete application or an arithmetic workload.
- One warmup per backend, seven measured samples, alternating backend order.
- Median wall time includes process creation, ELF loading and teardown.
- CPU time and peak RSS come from OS child-process accounting in a fresh worker.
  Peak RSS can include inherited process memory. Do not treat it as steady-state
  engine memory. In this Linux environment both engines reported the same RSS
  floor, so these results do not establish a memory advantage.
- Raw samples, host, compiler options and binary hash are saved in JSON.
- `--samples 15 --steps 1000000` can increase the sample count and dispatch work.

Do not extrapolate Linux timings to macOS or ratios to real applications.
No Docker/lightweight-VM baseline was measured. No JIT or direct guest execution
was implemented. Those remain separate parts of the broader backend research.

## Sanitizers (optional developer check)

```sh
python3 scripts/build_native.py --sanitize
python3 scripts/verify_m2.py --native build/vmfree-c-sanitize
```

The Linux validation here used AddressSanitizer and UndefinedBehaviorSanitizer.
LeakSanitizer could not inspect processes in this managed environment, so that run
used `ASAN_OPTIONS=detect_leaks=0`. Address/undefined-behavior checks passed on all
20 cases; leak checking remains unverified. Do not use sanitizer builds to benchmark.

## Decision

Keep Python as the reference specification. The compiled interpreter passes the
current conformance checks and is worth measuring on the Mac. Mac results should
precede decisions about a production core or extending instructions and memory.
The full roadmap still requires translation experiments, richer workloads and a
same-Mac VM baseline before claiming a performance improvement over Docker.

Measurement references:
- [Python resource accounting](https://docs.python.org/3/library/resource.html)
- [Apple getrusage manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html)
