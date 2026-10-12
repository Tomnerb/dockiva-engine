# M32: representative performance feasibility (in progress)

`scripts/benchmark_m32.py` measures the pinned dynamic shell startup and a verified
1 MiB HTTP fetch. Each run records wall time, user/system CPU time and the native
process's peak RSS via wait4, with fresh private filesystems and a bounded deadline.
It preserves raw samples and records the runtime hash. The local fixture server
is outside the measured process. The default run collected eight repetitions of
each workload in `evidence/m32-standalone-feasibility.json`.

These are engineering measurements for two small supported workloads. They do
not include managed supervisors, Docker Desktop's VM, whole-system idle cost,
energy or battery. No savings percentage is computed. Existing OrbStack timing
results remain separate from a future Docker Desktop comparison.

M32 remains OPEN: representative application stacks, agreed numerical budgets,
matched Docker Desktop measurements, full product/VM accounting, uncertainty
analysis and architecture feasibility review are not complete. RAM, CPU and
battery comparison remains part of final product qualification, as requested.
