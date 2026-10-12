# M35: instruction compatibility work in progress

The serialized interpreter now handles LDAR/STLR for byte, halfword, word and
64-bit accesses, additional loader SIMD operations, and architectural hints when
their optional features are absent. No optional CPU capabilities are advertised.
This is prerequisite work, not acceptance of M35 or shared-memory threading.

The added SIMD subset includes UZP1/2, integer MOVI with 16/32-bit lanes, scalar FMOV,
full-register and selected lane LD1/ST1, SIMD register-offset loads/stores,
CMEQ-zero, unsigned CMHI/CMHS, logical register/immediate operations, SHRN/SHRN2, ADDHN/ADDHN2 and vector ADDP.
Exclusive loads/stores also cover all four integer widths.
Reserved forms remain rejected. Guest memory permissions and stack alignment
checks still apply, including to vector accesses.

`examples/m35/acquire_release.S` checks results independently of the C decoder.
`scripts/verify_m35_acquire_release.py --linux` compares successful execution on
ARM64 Linux with normal and address/undefined-sanitized interpreter builds, with
both caches independently disabled. `verify_decode_cache.py` checks classification
and cache behavior; it does not independently establish instruction semantics.

M34 general shared-thread lifetime, concurrent reservation invalidation, adversarial
interleavings and M35 stress qualification remain open. The interpreter executes
one guest instruction stream at a time; these tests do not establish correctness
for a future parallel or translated backend.

Architecture references: [Arm instruction set reference](https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab),
[Arm memory ordering discussion](https://developer.arm.com/community/arm-community-blogs/b/tools-software-ides-blog/posts/armv8-sequential-consistency),
and [Arm engineer's GCS/CHKFEAT description](https://people.debian.org/~ema/minidebconf-Cambridge-2024-Slides/Capper.pdf).

Database startup follow-up adds unsigned vector multiply/widening addition,
scalar ADDP, CNT, ADDV, EXT, general-register lane insertion, raw S/W and D/X
transfers, and ADC/SBC flag handling. All remain subject to the scoped fixture
and Linux-reference checks; this is not a claim of complete ARM64 execution.

Further database prerequisites include vector lane INS/UMOV, multiple-structure
loads/stores with de-interleaving and post-indexing, EXTR, signed/unsigned widening
and fixed-width shifts, scalar DUP, UMINP, MOVI byte masks, signed pair loads and
non-temporal integer pairs. Prefetch hints do not access data in this interpreter.
Independent result tests cover these paths on Linux and normal/sanitized VM-free.
Scalar floating-point division remains unsupported and blocks Redis/Valkey.
