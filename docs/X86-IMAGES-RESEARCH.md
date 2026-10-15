# Running linux/amd64 images on Dockiva Engine: research report

Date: 2026-10-15. Scope: research only, no code changes. Engine source read at
`dockiva-engine/` (branch `v0.5-gaps`). Statements marked **[known]** are backed by a
cited source or by the engine source; **[inferred]** are my reasoning; **[unknown]** need
an experiment.

## TL;DR

- **Rosetta cannot legitimately be used outside a Virtualization.framework VM.** The
  technical gate is small: an `ioctl` on `/proc/self/exe` that our syscall layer could
  answer. The obstacles are the licence (no Rosetta-specific grant, and a no-reverse-engineering
  and no-modification clause), Apple changing the check between releases, and
  missing AVX. Use it only through a real VZ VM, if at all.
- **The best fit is a translator that runs as a guest program.** FEX-Emu (MIT, works at the
  syscall level so it doesn't care which libc an image uses, has AVX/AVX2, can use hardware TSO)
  would be the primary translator. Box64 (MIT) is the alternative, and QEMU user-mode (GPLv2) is
  the slow fallback for correctness. All three are arm64 Linux ELF programs that our
  runtime can already almost run.
- **The real work is in our runtime, not in a translator.** Every translator needs:
  - a general 47/48-bit address space (done 2026-10-15: `MAP_FIXED` anywhere, `mremap`);
  - the translator loaded high, out of the x86 program's way (done 2026-10-15: PIE at `0xaaaaaaaa0000`);
  - binfmt-style dispatch in `execve`;
  - a way to turn on Apple's hardware TSO per thread (`ACTLR_EL1` bit 1, which Hypervisor.framework exposes from
    macOS 15).
- **Writing our own x86-64 JIT is a multi-year programme.** It isn't worth doing.
- **First experiment:** run a static x86-64 hello world under `dockiva-hvf` with FEX, then with Box64
  (`-DSTATICBUILD`), each started directly with no binfmt, and log the first failing
  syscall or mmap. I expect it to fail on `MAP_FIXED` at 0x400000. That result alone
  confirms the plan below.
- **Experiment results (section 7):**
  - E2 is answered. Hardware TSO works through the public `HV_SYS_REG_ACTLR_EL1`; `PR_SET_MEM_MODEL` is
    now offered, and a litmus test shows millions of reorderings without TSO and none with it.
  - E1: both FEX and Box64 stop at start-up on fixed mappings outside the arena, before any x86 code
    runs. FEX's tools found four small gaps, now fixed.
  - Recommendation: **go with FEX**, not Box64, once the address-space work lands.
  - E1 re-run after the address-space merge (section 7.1):
    - **Box64 runs real x86-64 programs** under `dockiva-hvf`: glibc `python3.12` with threads and sqlite,
      a glibc `bash` pipeline that execs x86 children, static-PIE and musl `busybox echo`.
    - **FEX still stops at one call:** a fixed mapping inside the unused 8 MiB brk room.
  - 7.2: the brk room now takes fixed mappings. FEX gets past its start-up mapping and now stops only
    on its server's abstract AF_UNIX socket.
    - FEXServer still needs abstract AF_UNIX sockets.

---

## 0. What the engine does today (relevant facts)

All of the following is **[known]** from the engine source:

| Area | Current behaviour | Source |
|---|---|---|
| amd64 rejection | `Only linux/arm64 supported` in pull/load/run/build paths | `dockivad/image.go:164,171`, `dockivad/api.go:751`, `dockivad/build.go:73,498`, `oci.py:97`, `daemon.py:131` |
| ELF loader | Accepts only `e_machine == 183` (AArch64); since 2026-10-15 PIE main programs load at `0xaaaaaaaa0000` and PT_INTERP / static PIE just below `0xfffff8000000`, as Linux arm64 without ASLR (was `0x400000` and 60 GiB) | `native/runtime.c` (`load_stream_inner`), `native/address_space.h` |
| exec | `#!` scripts handled in `process_exec`, up to 4 levels deep. That loop is the natural binfmt hook | `native/process.h:92-121` |
| Page tables | Stage 1 uses a 4 KiB granule, 48-bit VA (T0SZ=16), 40-bit IPA. Guest pages are 4 KiB | `native/hvf_backend.h:152,481` |
| mmap | One arena at `0x300000000` of at most 12 GiB for non-fixed mappings. Since 2026-10-15: `MAP_FIXED` / `MAP_FIXED_NOREPLACE` anywhere in [64 KiB, `0xffffffff0000`) outside the arena (anonymous, private file, shared), replacing what was there; up to 65536 reservations, split by munmap holes; fresh PROT_NONE reservations in [16 TiB, 112 TiB). A fixed range across the arena's edge is still EINVAL, and hints outside the arena are ignored | `native/mapping.h` (`guest_dynamic_mmap`, `reserve_*`) |
| mremap | Implemented 2026-10-15: in-place grow/shrink, `MREMAP_MAYMOVE`, `MREMAP_FIXED`, `MREMAP_DONTUNMAP`, old size 0 on shared memory; private file mappings do not grow | `native/mapping.h` (`guest_mremap`) |
| JIT support | RWX mappings allowed under HVF; memfd double-mapping supported; membarrier supported | `native/runtime.c:1151`, `native/fs.h:492`, `native/hvf_backend.h:728` |
| Signals | SIGSEGV/SIGILL delivered to handlers with si_code/si_addr; arm64 sigcontext with FPSIMD record (`0x46508001`); sigaltstack | `native/hvf_backend.h:732-756`, `native/signals.h:179` |
| Threads/processes | 1024 threads per process, 63 processes | `native/scheduler.h:7`, `native/resources.h:7` |
| vDSO | Synthetic vDSO **with DT_HASH** (Rosetta asserts on this) | `native/vdso.h:10,75` |
| auxv | AT_HWCAP/HWCAP2 built from `hw.optional.arm.FEAT_*`. HWCAP_CPUID is not set, so translators detect features from auxv | `native/runtime.c:248-272` |
| prctl | No `PR_SET_MEM_MODEL`/`PR_GET_MEM_MODEL` | `native/compat.h:185-203` |
| /proc | `/proc/self/maps`, `auxv`, `cmdline`, `exe`, `status`… present | `native/fs.h:597`, `native/mapping.h:589` |
| Roadmap | M81 "AMD64 Linux image compatibility": a separate programme, "not a small patch" | `docs/ROADMAP.md` M81, `docs/FINAL-ARCHITECTURE-V1.md:35,143` |

Takeaway: the runtime already meets most of what a JIT needs: 4 KiB pages, RWX, signals
with ucontext, membarrier, memfd, `/proc/self/maps`. The 4 KiB page size matters because
FEX does not support 16 KiB-page kernels, which is why Asahi needs a microVM for it ([Fedora FEX
change](https://www.fedoraproject.org/wiki/Changes/FEX)). We already have 4 KiB pages. The
**memory map** is the main gap.

---

## 1. Rosetta

### 1.1 How Rosetta for Linux is delivered **[known]**

- Rosetta for Linux is an arm64 Linux ELF (`rosetta`), found on the host under
  `/Library/Apple/usr/libexec/oah/RosettaLinux/`. It is exposed to a guest only as a VirtioFS
  directory share, `VZLinuxRosettaDirectoryShare` (macOS 13+)
  ([Apple docs](https://developer.apple.com/documentation/virtualization/vzlinuxrosettadirectoryshare),
  [threedots.ovh](https://threedots.ovh/blog/2022/06/quick-look-at-rosetta-on-linux/)).
- The guest registers it with binfmt_misc using the x86-64 ELF magic/mask and the flags `F` (fix-binary), `C`
  (credentials) and `P` (preserve argv0; `no` on macOS 13)
  ([UTM docs](https://docs.getutm.app/advanced/rosetta/),
  [NixOS module](https://raw.githubusercontent.com/NixOS/nixpkgs/master/nixos/modules/virtualisation/rosetta.nix)).
- **AOT caching**: from macOS 14, `VZLinuxRosettaDirectoryShare.cachingOptions` with
  `VZLinuxRosettaUnixSocketCachingOptions` / `…AbstractSocketCachingOptions` turns on
  translation caching through a daemon in the guest
  ([Apple docs](https://developer.apple.com/documentation/virtualization/vzlinuxrosettaunixsocketcachingoptions)).
  Rosetta asks for the daemon's socket with a second ioctl (`'a'`, nr `0x23`, 0x80 bytes),
  which returns `/run/rosettad/rosetta.sock`
  ([zenn.dev write-up](https://zenn.dev/orimanabu/articles/rosetta-libkrun)). The
  `rosettad` protocol is not publicly documented. The 2022 analysis found Rosetta for
  Linux JIT-only. AOT came later.

### 1.2 The environment check **[known]**

- At startup Rosetta opens `/proc/self/exe` and issues
  `ioctl(fd, _IOC(_IOC_READ, 'a', 0x22, 0x45))`. It expects back a fixed 69-byte string, an Apple
  copyright notice that begins "Our hard work…". If the ioctl fails or the string doesn't match, it exits with
  *"Rosetta is only intended to run on Apple Silicon with a macOS host using
  Virtualization.framework with Rosetta mode enabled"*. VirtioFS passes the ioctl through to the
  host ([threedots.ovh](https://threedots.ovh/blog/2022/06/quick-look-at-rosetta-on-linux/),
  [zenn.dev](https://zenn.dev/orimanabu/articles/rosetta-libkrun)).
- libkrun once got past the check by serving a user-supplied key file. The support was
  removed: *"It's been broken for a while and we have no way of supporting it in a reasonable
  way"*. The same commit leaves room for FEX or Box64 instead
  ([libkrun PR #176](https://github.com/containers/libkrun/pull/176)).
- Apple changes this gate between releases. After the macOS 15 upgrade, Parallels users hit the
  same error message, and Docker and OrbStack had to ship fixes
  ([Parallels forum](https://forum.parallels.com/threads/rosetta-2-no-longer-available-under-macos-15-beta-in-linux-vms.365156/post-933012)).
- Other host requirements found by reverse engineering: FEAT_FlagM, and a CNTFRQ of 24 MHz or 1 GHz
  ([threedots.ovh](https://threedots.ovh/blog/2022/06/quick-look-at-rosetta-on-linux/)).
  Both hold under our HVF setup. Rosetta also needs DT_HASH in the vDSO
  ([Apple forum 769162](https://developer.apple.com/forums/thread/769162)), which our vDSO has.

### 1.3 Could our runtime run `rosetta` as a guest?

**Technically: probably yes [inferred].** Rosetta is an ordinary arm64 Linux ELF. Our
runtime controls the fd returned for `/proc/self/exe` and could answer ioctl 0x22, and 0x23 for
`rosettad`. It provides a DT_HASH vDSO and runs code on real Apple cores, so FlagM and
the 24 MHz counter are present. TSO is covered in 1.4. It would also need the same
address-space work as every other translator (section 6). **[unknown]** Rosetta may make
other environment checks (for example statfs on the share or other ioctls) that nobody has
published.

**Licence and product risk: do not ship [known + inferred].**
- The macOS Tahoe 26 SLA has **no Rosetta-specific grant**. It lets you run the Apple Software
  on Apple-branded computers. §2N forbids anyone to *"decompile, reverse engineer, disassemble … modify,
  or create derivative works"*, and §4B says components *"may not be separated from the
  bundle and distributed as standalone applications"*
  ([macOS Tahoe 26 SLA](https://www.apple.com/legal/sla/docs/macOSTahoe.pdf)). So we could
  never redistribute the binary, only use the copy the user has installed.
- Apple documents only one supported delivery path, the VZ directory share. The binary's own
  gate says it is meant only for VZ. Answering the key ioctl means reproducing Apple's
  guard string to get round a usage check. That is a circumvention argument we should not
  ship on, and the libkrun precedent shows maintainers judged it unsupportable. *(Not
  legal advice; get counsel before any Rosetta work.)*
- Apple says Rosetta for Intel *apps* ends after macOS 27, but it will keep supporting
  "software running Intel binaries in Linux VMs"
  ([MacRumors](https://www.macrumors.com/2026/02/16/macos-tahoe-26-4-rosetta-2-warnings/)).
  That supported future is VZ-only.
- **AVX:** Rosetta for Linux does not advertise AVX/AVX2. `ROSETTA_ADVERTISE_AVX` works
  on macOS but not in Linux VMs ([Apple forum 769486](https://developer.apple.com/forums/thread/769486)).
  x86-64-v3 images such as RHEL 10 and CentOS Stream 10 fail with *"CPU does not support
  x86-64-v3"* under Rosetta ([RHEL-33471](https://issues.redhat.com/browse/RHEL-33471),
  [Red Hat KB](https://access.redhat.com/solutions/7122807)).

**The legitimate Rosetta option** is to start a small VZ Linux VM *only for amd64 containers*,
with `VZLinuxRosettaDirectoryShare` and caching options, as OrbStack, Docker Desktop and Apple's
`container --rosetta` do
([apple/container command reference](https://github.com/apple/container/blob/main/docs/command-reference.md),
[OrbStack](https://docs.orbstack.dev/docker/)). It is fast (about 71% of native on 7-zip, section 3) and
licensed, but it breaks the "no VM" principle. Keep it as a product-level fallback, not part of the
engine.

### 1.4 TSO, which matters for every translator

- Apple cores have a per-CPU TSO mode, `ACTLR_EL1` bit 1 (`ACTLR_APPLE_TSO`). Availability is shown by
  `AIDR_EL1` bit 9 ([kernel patch 4/4](https://lkml.rescloud.iu.edu/2404.1/04830.html)).
- Apple documents a Linux kernel patch for VMs that context-switches `ACTLR.TSOEN` per thread and adds
  `prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO)`. It notes that without per-process control the whole VM
  runs in TSO ([Apple: Accelerating the performance of Rosetta](https://developer.apple.com/documentation/virtualization/accelerating-the-performance-of-rosetta)).
  The upstream proposal uses `PR_SET_MEM_MODEL=0x4d4d444c`, `PR_GET_MEM_MODEL=0x6d4d444c`, TSO=1
  ([patch 1/4](https://lkml.rescloud.iu.edu/2404.1/04826.html)). VM guests can change TSOEN from
  macOS 14.4 ([patch series](https://lkml.iu.edu/hypermail/linux/kernel/2404.1/04656.html)).
- Hypervisor.framework exposes **`HV_SYS_REG_ACTLR_EL1` from macOS 15.0**
  ([Apple docs](https://developer.apple.com/documentation/hypervisor/hv_sys_reg_t)). Our runtime
  owns EL1, so it can set the bit per vCPU and per guest thread. **[unknown]** Whether HVF accepts
  writes to bit 1 through `hv_vcpu_set_sys_reg`, or only through an `MSR` executed at guest EL1. That is
  experiment E2.
- FEX turns on hardware TSO through this prctl when the kernel offers it, and without it falls back to slow
  software emulation ([FEX-2306](https://fex-emu.com/FEX-2306/)). **If HVF refuses the bit**, the
  fallback that stays correct is to run all threads of a translated process on one vCPU at a time. That
  is correct under TSO but loses parallelism. Our scheduler could enforce it.

---

## 2. QEMU user-mode (`qemu-x86_64` as an arm64 guest)

**Feasibility [inferred, high].** QEMU's linux-user mode is a self-contained arm64 program, usually built
static. It is libc-agnostic, so musl, glibc and static Go binaries all work. It handles threads by creating one host
thread per guest `clone`, and only "a limited set of flags" is accepted
([QEMU user docs](https://www.qemu.org/docs/master/user/main.html)). The team already uses
`qemu-user-static` 7.2 as a reference oracle (`VALIDATION.md`).

**What it needs from our runtime:**
- `MAP_FIXED` file and anonymous mappings at the guest's addresses, or at `guest_base`+address. `-B`/`-R`
  can move the whole guest space into one reservation, but our reservations only take
  anonymous commits today.
- `/proc/self/maps` to find free space, which we have.
- An RWX code_gen_buffer, which we allow.
- Host SIGSEGV for self-modifying-code detection, with `mprotect` on guest code pages.
- `clone(CLONE_VM|CLONE_THREAD|…)`, futex and signals, all of which we have.
- `QEMU_STRACE` makes a free syscall tracer for debugging.

**Performance [known].**
- Slowest of the options. 7-zip on a Pi 400: QEMU x86_64 931 vs native 5787, about **16%, ~6× slower**
  ([box86.org](https://box86.org/2022/03/box86-box64-vs-qemu-vs-fex-vs-rosetta2/)).
- Fedora calls it "extremely slow in comparison" with FEX and Box64
  ([Fedora FEX change](https://www.fedoraproject.org/wiki/Changes/FEX)).
- QEMU emits fences for x86 ordering, so it is **correct without hardware TSO** but pays for it **[inferred]**.
- AVX/AVX2/FMA/F16C have been in x86 TCG since 7.2
  ([QEMU 7.2](https://qemu.org/2022/12/14/qemu-7-2-0)).

**Licence [known + inferred].** QEMU is GPLv2
([QEMU docs](https://www.qemu.org/docs/master/user/main.html)). It runs as a separate guest program
that talks only through the syscall ABI. That is mere aggregation, so our C/Go/Swift code is not
affected.
- **Bundling it in Dockiva.app** means we distribute it. We must ship the licence text and the matching
  source, or a written offer, for the exact build.
- **Downloading on demand** from a third-party distributor (for example Debian's
  `qemu-user-static`) leaves the source obligation with that distributor. If *we* host the binary, the
  obligation is ours again.
- Either way it is manageable. Docker and OrbStack ship QEMU.

**Role:** fallback for correctness, and the reference oracle. Not the default.

---

## 3. Other translators

| | **FEX-Emu** | **Box64** | **Blink** | QEMU user | Rosetta (VZ only) |
|---|---|---|---|---|---|
| Licence | MIT | MIT | ISC | GPLv2 | Apple SLA |
| Approach | JIT, emulates at the Linux **syscall** level; libc-agnostic | JIT (dynarec) plus **wrapping native arm64 libs** (libc, libm, GL…) | Interpreter plus small JIT, syscall level, ~200 KB | TCG JIT, syscall level | AOT plus JIT |
| x86-32 | yes | no (Box86) | yes | yes | yes |
| AVX/AVX2 | yes since FEX-2407, also on ASIMD-only cores ([FEX-2407](https://fex-emu.com/FEX-2407/)) | yes since v0.3.0, NEON ([v0.3.0](https://newreleases.io/project/github/ptitSeb/box64/release/v0.3.0)) | **no**, README lists up to SSSE3/POPCNT/BMI2 ([blink](https://github.com/jart/blink)) | yes (7.2+) | **no in Linux VMs** |
| HW TSO | uses `PR_SET_MEM_MODEL` ([FEX-2306](https://fex-emu.com/FEX-2306/)) | relies on its own strong-memory heuristics; I found no use of Apple TSO **[unknown]** | no | uses fences instead | yes |
| Speed (7-zip, % native) | 26% (Pi, 2022); now "similar to box64" ([Fedora](https://www.fedoraproject.org/wiki/Changes/FEX)) | 53% (Pi), **57% on M1/Asahi** | QEMU-class; claims ~2× QEMU on some benchmarks | 16% (Pi) | **71% on M1** |
| Page size | **4 KiB only** (we have 4 KiB) | 4 KiB; limited 16 KiB hacks | any | any | 4/16 KiB |
| Container fit | Container image *is* the x86 rootfs. FEX itself needs its arm64 libs and `FEXServer` | `-DSTATICBUILD` exists "to use inside of a x86_64 chroot… in a docker image", but is "highly experimental" ([COMPILE.md](https://github.com/ptitSeb/box64/blob/main/docs/COMPILE.md)). Built around glibc, so musl/Alpine and static binaries are less proven **[unknown]** | Static, tiny, easy | Static, easy | n/a |

Sources for the speed column: [box86.org benchmark (2022)](https://box86.org/2022/03/box86-box64-vs-qemu-vs-fex-vs-rosetta2/)
(Pi 400: native 5787, Box64 3084, FEX 1486, QEMU 931; M1: macOS native 47239, Rosetta 33746,
Asahi native 43271, Box64 24746). The numbers are old. FEX reports large gains since then, for example
+50–65% on some Geekbench tests in late 2023.

### How each would fit

- **FEX [best fit for containers].**
  - It handles x86 syscalls itself and issues arm64 syscalls to us. It is libc-agnostic, which covers Alpine,
    distroless and static Go.
  - With the container root presented as `/`, x86 `ld-linux-x86-64.so.2` and `/lib64` resolve
    naturally, so no separate x86 rootfs is needed. **[inferred]** Configure FEX with no RootFS, or RootFS=`/`.
  - The cost is FEX's own arm64 dependencies. FEXInterpreter is dynamically linked (glibc, libstdc++) and its
    PT_INTERP points to `/lib/ld-linux-aarch64.so.1`, which doesn't exist in an x86 image. Options:
    1. build FEX with `RUNPATH=$ORIGIN/../lib`, and have our loader override PT_INTERP for the
       translator to a hidden read-only mount such as `/.dockiva/x86/lib/ld-linux-aarch64.so.1`; or
    2. try a static-PIE FEX build. I found no supported option for this **[unknown]**.
  - FEXServer is required ([FEX-2207](https://newreleases.io/project/github/FEX-Emu/FEX/release/FEX-2207)).
    FEXInterpreter starts it automatically (fork, Unix socket). We support both, but it adds one hidden process per
    container.
  - FEX wants the upper half of the 48-bit space for itself, because x86-64 user space is 47-bit. That needs high
    `MAP_FIXED`/reservations ([FEX release notes](https://newreleases.io/project/github/FEX-Emu/FEX/release/FEX-2207)).
  - Long term, FEXCore is already used as a pure CPU-emulation library: Wine's ARM64EC/WoW64 modules do
    syscall translation outside FEX ([FEX wiki](https://wiki.fex-emu.com/index.php/Development:ARM64EC)).
    That would let us embed FEXCore later and own the x86 syscall ABI. It is a big project and not needed first.
- **Box64.**
  - The fastest in published numbers, and simplest to deploy as one static binary.
  - Its speed comes from wrapping native libs, mostly for GL, SDL and Vulkan, which server containers rarely use.
  - The static build wraps only libc, libm and libpthread, and is experimental.
  - Risk: musl and static x86 binaries, which are common in containers, are its least-tested path.
- **Blink.** Small and easy, but no AVX and no SSE4.x listed, so x86-64-v2 images (RHEL 9/UBI 9) and v3 images
  fail **[inferred from README]**. The latest tagged release is 1.1.0. Fine as a toy, not for real images.

---

## 4. Writing our own x86-64 → arm64 JIT

Rough scope **[inferred]**:

- x86 decoder covering the ISA: legacy, REX, VEX (AVX/AVX2), x87 80-bit, SSE1–4.2, BMI, and about 1,500
  instruction forms.
- EFLAGS semantics, with lazy flags tuned to the arm64 NZCV/FlagM.
- Segment bases (FS/GS for TLS).
- Self-modifying-code detection and invalidation.
- TSO mapping.
- Precise faults and signal frames in **x86** `ucontext` layout.
- An x86-64 → arm64 syscall ABI layer: different numbers; different struct layouts for `stat`, `epoll_event`
  (packed on x86), `sigaction`, `ucontext`, ioctls, `ptrace`, vsyscall/vDSO.
- CPUID emulation.
- A correctness corpus.

For comparison:
- QEMU's x86 TCG frontend is tens of thousands of lines and is the slow one.
- FEX has been in development since 2019, with several full-time engineers.
- Box64 has been in development since 2021.
- Apple built Rosetta with hardware help: TSO and FlagM2.

Estimates:
- An interpreter that can run Alpine `busybox sh` correctly: **6–12 engineer-months**.
- A JIT within 2× of FEX or Box64 on real images, including AVX2: **several engineer-years**, plus open-ended upkeep.

It is not worth it. Both MIT translators are open source, run as ordinary arm64 Linux programs, and can be
embedded or forked. The scarce engineering resource is the Linux personality, which we need either way.

---

## 5. Recommendation

**Strategy:** treat x86 support as *"run an MIT-licensed translator as a guest program"*.
- Make **FEX** the default.
- Keep **Box64** as a benchmarked alternative.
- Make **QEMU** an optional download for correctness fallback and as a test oracle.
- Do not use Rosetta in the engine. If a product decision allows it, offer "amd64 through a VZ helper VM with
  Rosetta" as a separate option.
- Restrict amd64 to the HVF backend. A JIT running under our arm64 interpreter would be impractically slow.

### Ordered plan with effort estimates (one engineer familiar with the runtime)

| # | Work | Effort | Exit criterion |
|---|---|---|---|
| E1 | **Bake-off experiment** (below): FEX and Box64-static run as direct guests on a static x86 hello world; list every failure | 1–3 days | A ranked list of runtime gaps per translator |
| E2 | **TSO probe:** set `ACTLR_EL1` bit 1 through `hv_vcpu_set_sys_reg(HV_SYS_REG_ACTLR_EL1)` on macOS 15+, and separately through `MSR` from the EL1 vector stub; run a message-passing litmus test with and without it | 1–2 days | Yes/no on hardware TSO under HVF |
| 1 | **General address space:** `MAP_FIXED`/`NOREPLACE` and file-backed fixed mappings anywhere in [64 KiB, 2^48 − system VA); file-backed commits inside reservations; `mremap`; raise or remove the 8-reservation and 12 GiB limits for translated processes; place translator PIEs high (Linux arm64 `ELF_ET_DYN_BASE` ≈ `0xAAAA_AAAA_B000`, above the x86 47-bit window) | 2–4 weeks | x86 `ET_EXEC` at 0x400000 and x86 `ld.so` map correctly under FEX |
| 2 | **binfmt dispatch:** in `process_exec` (next to the `#!` loop) and in the initial exec, detect `e_machine==62` and run `/.dockiva/x86/<translator> [args] <path> argv…` with `F`/`P` semantics; set `/proc/self/exe` and `AT_EXECFN` the way the translator expects; add a hidden read-only bundle mount; override PT_INTERP for the translator | 1 week | `sh -c 'ls \| wc'` in `alpine:latest` amd64 crosses exec boundaries |
| 3 | **TSO wiring:** `PR_SET_MEM_MODEL`/`PR_GET_MEM_MODEL` → per-guest-thread `ACTLR_EL1` bit, context-switched by the scheduler. If E2 says no, pin translated processes to one vCPU | 3–5 days | Multithreaded x86 litmus tests pass; FEX reports hardware TSO |
| 4 | **Control plane:** allow `linux/amd64` in `dockivad` (`image.go`, `api.go`, `build.go`) and `oci.py`/`daemon.py`; platform order arm64 → amd64; report "emulated" in Inspect and the UI; profile selection by image digest (existing profiles mechanism) | 3–5 days | `docker run --platform linux/amd64 alpine uname -m` prints `x86_64` |
| 5 | **Compatibility hardening:** amd64 variants of the acceptance corpus (alpine, debian, ubuntu, nginx, redis/valkey, postgres, node, python, a static Go binary, an x86-64-v3 image such as UBI 10); fix syscall and `/proc` gaps | 3–6 weeks | Corpus parity with arm64 on correctness |
| 6 | **Distribution and performance:** pin a FEX release and build it reproducibly (MIT notices); optional QEMU download; benchmark against OrbStack/Rosetta; consider FEX code caching | 1–2 weeks | Documented performance; shippable bundle |
| (later) | Embed FEXCore as a library and own the x86 syscall ABI | months | Only if the guest-translator approach caps performance |

Total to a preview: **about 2–3.5 months** after E1/E2, mostly items 1 and 5. Item 1 also helps
arm64 workloads that use high `MAP_FIXED` (JVMs, sanitizers, Go race detector).

### First experiment (E1): exact recipe

Goal: find out, in a day, what blocks a translator on our runtime. No product code changes are
needed beyond a debug flag at most.

1. On an arm64 Linux machine or an arm64 container:
   - Build **FEX** (release tag) with `RUNPATH=$ORIGIN/../lib`, and collect `FEXInterpreter`, `FEXServer`
     and their `.so` dependencies, including `ld-linux-aarch64.so.1`.
   - Build **Box64** with `-DARM_DYNAREC=ON -DSTATICBUILD=ON` (Apple M-series target flags as documented).
   - Build two x86-64 test programs: a `musl`-static hello world and a glibc-dynamic hello world, taken with its
     `/lib64` from `debian:bookworm` amd64.
2. Build a test rootfs:
   - the x86 files at their normal paths;
   - translator trees under `/.dockiva/x86/{fex,box64}/`.
3. Run each translator under `build/dockiva-hvf` as the *main program*, calling it explicitly with no binfmt:
   - `/.dockiva/x86/fex/lib/ld-linux-aarch64.so.1 /.dockiva/x86/fex/bin/FEXInterpreter /hello-static`
   - `/.dockiva/x86/box64/box64 /hello-static`
4. Record the first unsupported syscall, mmap failure or fault (runtime stderr; `FEX_*`/`BOX64_LOG=2`
   logging).
   - **Expected:** `MAP_FIXED` at `0x400000`, or a high reservation, returns EINVAL/ENOMEM, because PIE
     translators currently load at 4 MiB and fixed mappings outside the arena are refused.
   - Patch locally behind a flag (load the translator at a high base; allow fixed anonymous mappings outside the arena) until
     `hello` prints. Then repeat with the dynamic glibc hello.
5. In the same run, do E2: flip `ACTLR_EL1` bit 1 and run a two-thread message-passing litmus test natively on arm64.
   It should show reordering without TSO and none with it.

Success means at least one translator prints from both hellos, and we have a complete gap list. That
list sizes items 1–3 precisely.

---

## 7. Experiments (E1, E2), 2026-10-15

Host: M2 Max, macOS 26.5.1, `build/dockiva-hvf` from `rt-x86` (base `07bb976`), run directly with the
flags dockivad passes (`--dynamic --process-tree --signals --scheduled-network --outbound --cpus 4
--elf-limit-mib 1024 --mmap-limit-mib 12288 --storage-limit-mib 0 --writable`) and `--trace` for the
syscall log. Guest root: `ubuntu:24.04` arm64 with the translators added, and the x86 test files under
it (no dockivad change, no binfmt). Downloads, about 86 MB in all, deleted afterwards: `ubuntu:24.04`
arm64 (28.9 MB), `alpine` / `busybox` / `busybox:musl` / `python:3.12-slim` amd64 (3.8 + 2.2 + 0.9 +
43.3 MB), `fex-emu-armv8.4_2610-1~n_arm64.deb` from the official PPA (`ppa:fex-emu/fex`, 3.4 MB),
`box64_0.4.3-3+dfsg-1_arm64.deb` from Debian sid (3.6 MB). node was not fetched: nothing got far
enough for it to add information.

Test programs: a hand-made 216-byte static x86-64 `ET_EXEC` at 0x400000 (write + exit), static-PIE
`busybox:musl`, glibc-dynamic `python3.12` (`python:3.12-slim`), musl-dynamic `busybox` (alpine).

### E1 result: both translators stop in their own start-up, before any x86 instruction runs

Both stop on the general-address-space gap (plan item 1). The other work on that gap must cover
exactly these calls, which the runtime refused **[known, traced]**:

| Translator | Call (from `--trace`) | Result | Effect |
|---|---|---|---|
| Box64 0.4.3 (Debian, `ET_EXEC` linked at 0x34800000, which loads fine) | `mmap(0x30000000, 0x1000, PROT_RWX, MAP_PRIVATE\|MAP_ANONYMOUS)`: a hint outside the arena | ignored, returns 0x300002000 in the arena | Box64 unmaps it and asks again with `MAP_FIXED` |
| | `mmap(0x30000000, 0x1000, PROT_RWX, MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_FIXED)` (flags 0x32) | **EINVAL** | "cannot allocate 0x1000 aligned bytes for bridge" (BOX64, then BOX32), then SIGSEGV at pc 0x34c41f48, address -1 |
| FEX 2610 (`FEX`, PIE) | `mmap(0xF00011000, 0x1000, PROT_NONE, MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_NORESERVE\|MAP_FIXED_NOREPLACE)` (flags 0x104022): a guard page at the end of brk, which this runtime places after the interpreter at 60 GiB | **ENOMEM** | "Couldn't allocate page after SBRK.", then FEX's deliberate trap (SIGILL at 0x6fe67c) |
| FEX | (not reached) PIE main programs load at 0x400000 here, which is where every x86 `ET_EXEC` must go | - | high PIE base needed (plan item 1) **[inferred from the loader]** |

The same stop happens for every test program, because neither translator gets as far as opening it.
Those calls are the whole list: nothing past them could be observed. Getting further needs the
address-space work. A local experiment-only patch (fixed mappings anywhere, high PIE base) was not
allowed in this task, so **E1 must be re-run once that work lands**. Other things seen on the way, none
of them blocking:

- `set_robust_list`, `rseq`, `clone3`: ENOSYS. glibc copes.
- `prctl(PR_SET_VMA)`: EINVAL, as a kernel without `CONFIG_ANON_VMA_NAME` returns.
- `madvise(MADV_FREE)`: EINVAL, which is deliberate here.
- Missing sysfs files: `/sys/devices/system/cpu` (the directory), `.../cpu0/cpufreq/cpuinfo_max_freq`
  and `/sys/kernel/mm/transparent_hugepage/enabled`. Box64's `popen("lscpu")` reports "failed to
  determine number of CPUs" and falls back.

FEX's helper programs run further, and found four runtime gaps. All four are **fixed on this branch**:

| Gap | How it showed | Fix (files) | Test |
|---|---|---|---|
| EL0 `MRS` of the ID registers (`MIDR_EL1`, `ID_AA64ISAR0/1`, `PFR0`, `MMFR2`…). Linux emulates these reads; FEX and FEXGetConfig read them unconditionally | SIGILL, ESR class 0x18, at `mrs x20, ID_AA64ISAR0_EL1` | `hvf_id_register` emulates them as Linux does: only the user-visible fields (no PAuth/BTI); MPIDR = 0x80000000; the rest of the ID space reads 0. `HWCAP_CPUID` is now set (`hvf_backend.h`, `runtime.c`) | `tests/native/translator_guest.c`: fails at check 1 before, passes after |
| Unaligned atomics: a data abort with DFSC 0x21 | taken as "stale page tables", then SIGSEGV. A write would have looped on the copy-on-write path | SIGBUS `BUS_ADRALN` at the address, as Linux. FEX backpatches on it (`hvf_backend.h`) | `translator_guest.c` (`ldar`/`stlr` across 16 bytes) |
| `prctl(PR_GET_MEM_MODEL / PR_SET_MEM_MODEL)` | EINVAL, so FEX would fall back to software TSO | per-thread TSO: see E2 (`compat.h`, `cpu_state.h`, `hvf_backend.h`) | `tests/native/tso_guest.c` |
| `lstat("/proc/self/exe")` reported a regular file | FEXServer: libstdc++ `read_symlink` throws EINVAL, so `abort()` | `/proc/<pid>/exe` lstat reports a symlink (`fs.h`) | `verify_procfs.py`: `stat -c %F` said "regular file" before, "symbolic link" after |

After these fixes, `FEXGetConfig -e` runs to completion under the runtime and reports, among other things:
- `Memory atomics emulation method: LSE`
- `Unaligned atomic memory granularity: 16-byte`
- `GPR memory model emulation: Hardware TSO` (and the same for memcpy and vector)
- `Fault Granularity: Split every 16 bytes`
- `midr=0x610f0000`, `isar0=0x21100110212120`

So FEX finds hardware TSO, LSE2 and correct SIGBUS behaviour, as it would on Asahi.

**A FEX gap still open:** FEXServer `bind`s an **abstract** AF_UNIX socket (`"\0" "0.FEXServer.Socket"`).
The runtime returns EOPNOTSUPP for abstract names, so FEXServer exits with "Failed to create FEXServer
socket: error 95". Clients try the abstract name first and then `/tmp/0.FEXServer.Socket`. Abstract names
need a per-container namespace that both the main run and `docker exec` runs can see, for example host
socket files named by a hash of the abstract name, in a directory keyed by the container. That is a
small design decision, so it is left for later.

**Box64 observations:**
- It is linked as glibc-dynamic (`ld-linux-aarch64.so.1`, libc, libm, libstdc++).
- It needs **x86** `libgcc_s` and `libstdc++` (Debian `Depends:`).
- It wraps the **arm64** libc. An amd64 image therefore needs both an arm64 glibc set and an x86 one;
  how that works for musl/Alpine images is **[unknown]**.
- Its dynarec reports ASIMD, AES, CRC32, PMULL, ATOMICS, SHA1, SHA2, USCAT, FLAGM, FLAGM2 and FRINT
  from our auxv.
- It never calls `PR_SET_MEM_MODEL`; the binary contains no such prctl. It orders memory with its
  `BOX64_DYNAREC_STRONGMEM` heuristics, which are off by default and switched on for JVM, TBB and Mono.
  Without them, multithreaded x86 code runs on the weak model.
- It needs fixed low mappings at 0x30000000 (below the arena, under 4 GiB) for its bridges.

### E2 result: hardware TSO works through the public Hypervisor.framework API

- `HV_SYS_REG_ACTLR_EL1` (0xc081) has been public since **macOS 15.0**. The SDK header says it "only allows
  getting / setting of the ACTLR_EL1.EnTSO bit (index 1)". No private API is used. Setting the bit through
  `MSR` from our EL1 stub was not needed and was not tried.
- The probe is in `hvf_cpu_new`: set bit 1, read it back, clear it. It works on this machine and sets
  `guest_tso`.
- Wiring:
  - `GuestCpuState.mem_model` holds the model per thread. `fork` and `clone` copy it, and exec clears it.
  - `hvf_put_regs` writes `ACTLR_EL1` only when the incoming thread's model differs from what that vCPU
    last had. The fast path pays one compare; switching between a TSO thread and a weak thread costs one
    `hv_vcpu_set_sys_reg`.
  - `prctl(PR_SET_MEM_MODEL=0x4d4d444c, 0|1)` and `PR_GET_MEM_MODEL=0x6d4d444c` follow the Apple/Asahi
    ABI that FEX uses. They return EINVAL where TSO is unavailable: the interpreter backend, macOS < 15,
    or other values.
- **Litmus test** (`tests/native/tso_guest.c`, run by `verify_hvf.py`): message passing between two
  processes on two of 4 vCPUs. The writer does `str x; str y` (2,000,000 rounds, separate cache lines);
  the reader does `ldr y; ldr x` and counts `x < y`.

  | Model | Reorderings counted per run (11 runs) |
  |---|---|
  | default (weak) | 4.5 M – 8.9 M |
  | TSO (`PR_SET_MEM_MODEL`, 1) | **0** in every run |

  The probe's verifier requires weak > 0, which shows the probe can see reordering, and TSO == 0.

This removes the "pin translated processes to one vCPU" fallback. Plan item 3 (TSO wiring) is done.

### 7.1 E1 re-run after the address-space merge (`rt-mmap`), 2026-10-15

What was re-run:
- Same root, the same downloads (fetched again, about 86 MB, then deleted) and the same four programs,
  on `main` at `57446db`.
- `main` now has MAP_FIXED/NOREPLACE anywhere in [64 KiB, 0xffffffff0000), growable reservations,
  `mremap`, PIE at 0xaaaaaaaa0000 and ld.so just below 0xfffff8000000.
- No experimental mapping patch.
- The harness now passes the guest path as `argv[0]` (`--argv0`), as dockivad does. Without it Box64
  resolved the host path and crashed in `strlen(NULL)`.

**Box64 0.4.3 now runs x86-64 code.**
- One more gap blocked it at first: `mmap(NULL, 0x40000, PROT_READ|PROT_WRITE,
  MAP_PRIVATE|MAP_ANONYMOUS|MAP_GROWSDOWN)` (flags 0x122) returned **EOPNOTSUPP**. Box64 allocates its
  x86 stack that way; it got -1 and crashed. Linux accepts the flag, so `mapping.h` now does too: it maps
  the range as asked, with no downward growth. That is a one-line flag change, tested in
  `tests/native/translator_guest.c`, which fails at check 16 before the change and passes after.
- Results after it:

| x86-64 program | Box64 | FEX 2610 |
|---|---|---|
| static `ET_EXEC` hello at 0x400000 | prints, rc 0. The image went into the arena instead (hint ignored); this worked only because the code is position-independent | stops at start-up (below) |
| `busybox:musl` (static-PIE) `echo hi` | `hi`, rc 0 | stops at start-up |
| same, `sh -c 'echo abc \| busybox wc -c'` | rc 139 with no output. Box64 warns "not really compatible with statically linked binaries"; one fixed mapping in the brk room failed on the way (below) | stops at start-up |
| glibc `python3.12 -c 'print(6*7)'` | `42` | stops at start-up |
| glibc `python3.12`: 4 threads hashing, sqlite3 with 1000 rows, json | `x86_64 4 499500 {"ok": true}` in 0.04 s | stops at start-up |
| glibc `bash -c 'echo abc \| wc -c; uname -m'` (x86 children exec'd) | `4`, `x86_64`, `5.2.37(1)-release` | stops at start-up |
| musl `busybox echo hi` (alpine) | `hi`, plus relocation errors for `ns_*` and `crypt`: Box64 binds musl symbols to the wrapped glibc | stops at start-up |

- **Box64's library wrapping does not hold up across image versions.** It replaces the image's x86
  OpenSSL with the host root's arm64 OpenSSL 3.0. Python's `_ssl` and `_hashlib` need `OPENSSL_3.3.0` and
  `OPENSSL_3.4.0` symbols (`X509_STORE_get1_objects`, `EVP_MD_CTX_get_size_ex`), so `import ssl` fails
  (hashlib falls back to its built-in code). `BOX64_EMULATED_LIBS=libssl.so.3:libcrypto.so.3` did not
  help. Every image would then need arm64 libraries whose versions match its own.
- Box64 keeps asking for fixed addresses that it does not get, and copes each time:
  - `@0x30000000`: its bridges. MAP_FIXED works now.
  - `@0x100000000`, `@0x7fff00000000`, `@0x7fff01000000`: hints for the PIE program and libraries, which
    land in the arena.
  - "could not allocate text video memory".

**FEX 2610 is still blocked, now by a single call.**
- brk(0) is 0xaaaaab12a000 (right after FEX at the new PIE base). FEX then calls
  `mmap(0xaaaaab12a000, 0x1000, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE)`
  (flags 0x104022), which returns **ENOMEM**. It prints "Couldn't allocate page after SBRK." and traps
  (SIGILL at 0xaaaaaad9e67c).
- Cause: `guest_dynamic_mmap` (`mapping.h`) refuses any fixed mapping inside the heap's 8 MiB room
  (`/* the heap's room */`), even where brk has not grown.
- Linux maps it. A later brk that would run into it fails and returns the old break; that is exactly what
  FEX wants, since it places this page to stop the host's brk from growing into guest memory.
- Box64's static busybox hits the same refusal: `mmap(0x3759b000, 0x1000, PROT_NONE,
  MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED)` returns ENOMEM. That address is just after Box64's own brk.
- **Fix for the address-space owner:**
  - allow fixed mappings in `[brk_base+brk_pages, brk_base+BRK_CAP)`;
  - make `guest_brk` refuse to grow over any segment or reservation.
  - That is a few lines, but they are in the address-space code, so this task did not make them.
- FEXServer (`FEXServer -f`) still exits 255 with "Failed to create FEXServer socket: error 95". Abstract
  AF_UNIX sockets are in progress elsewhere. FEX cannot be tried without the server until the SBRK call
  passes: it stops before connecting.
- `FEXGetConfig -e` still reports LSE, 16-byte unaligned-atomic granularity and hardware TSO at the new
  addresses.

Still not blocking: `/sys/devices/system/cpu` is not a directory, so Box64's `popen("lscpu")` prints
"failed to determine number of CPUs" on every start.

### 7.2 Fixed mappings in the brk room; E1 re-run, 2026-10-15

What changed in the runtime (on `main` `b663f9f`):
- `guest_dynamic_mmap` (`mapping.h`) now refuses fixed mappings only over the heap in use,
  `[brk_base, brk_base+brk_pages)`. MAP_FIXED and MAP_FIXED_NOREPLACE at any protection are allowed in
  the rest of the 8 MiB room.
- `guest_brk` (`heap.h`) refuses to grow over any mapping or reservation and returns the old break, as
  Linux does.

Tests (`verify_address_space.py` runs them on both backends, as part of `tests/native/wide_mmap_guest.c`,
now 222 checks):
- NOREPLACE guard page right at the break;
- brk refused at it;
- MAP_FIXED further up the room;
- NOREPLACE over the heap in use returns EEXIST;
- after `munmap` of the guard, brk grows again.

Before the change it failed at check 217, the guard page.

**FEX 2610: it now stops only at the abstract socket.**
- The brk(0) guard mapping succeeds, and FEX starts FEXServer itself.
- Every program then ends the same way, with exit 255:
  ```
  E Couldn't connect to FEXServer socket 0.FEXServer.Socket 95
  E Couldn't connect to FEXServer socket /tmp/0.FEXServer.Socket 2
  E Couldn't connect to FEXServer socket after launching the process
  E FEXServerClient: Failure to setup client
  ```
  The server it launched has already exited ("Failed to create FEXServer socket: error 95").
- `FEX_SERVERSOCKETPATH=/tmp/fex.sock` does not avoid this: FEX tries that name as an abstract address
  first, and the server still binds it abstract.
- FEX cannot run without its server. Once abstract AF_UNIX sockets land (another agent's work), re-run
  this section. There is no other known runtime blocker before FEX executes x86 code.

**Box64 static `busybox:musl sh -c 'echo abc | busybox wc -c'`: still rc 139**, but no longer because of
our mapping.
- Box64 crashes in its handler for the x86 program's SIGCHLD (`sigaction handler for sig 17`). It warns
  that statically linked binaries are "not really compatible".
- On the dynamic path the same signal machinery works: glibc `bash` runs a `trap … USR1` handler, a
  background job with `wait` (SIGCHLD) and a pipeline correctly (`got-usr1`, `4`, `0`).
- On the way it calls `mmap(0x37599000, 0x1000, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED)` and
  still gets ENOMEM. This is musl mallocng's guard page, placed with MAP_FIXED **over the heap it has just
  grown** with brk. Linux replaces the heap page; here the heap in use cannot be partly remapped.
  - musl ignores the failure; it only loses the guard page, and arm64 Alpine programs already run that way.
  - Recorded as P2 for the address-space owner: replacing pages of the heap in use would need the heap
    split into segments.

### Gaps by priority (what remains for an amd64 preview)

Updated after 7.1. The original P0 list (MAP_FIXED anywhere, high PIE base, mremap) has been merged.

1. ~~**P0, FEX: fixed mappings in the unused brk room.**~~ Done in 7.2.
2. **P0, abstract AF_UNIX sockets** (FEXServer; in progress elsewhere). This is now the only known blocker
   before FEX runs x86 code.
3. **P1, honour mmap hints outside the arena.**
   - Box64 asks for x86 `ET_EXEC` images at their link address with a hint and then relocates them. A
     non-PIE x86 program (most older and distro `ET_EXEC` binaries) needs its exact address.
   - FEX maps such images with MAP_FIXED, so this is mainly a Box64 need.
   - Non-fixed mappings that are not PROT_NONE also stay in the 12 GiB arena. That limits x86 address-space
     layouts that expect 47-bit placements.
3. **P1, binfmt dispatch and the hidden translator mount** (plan item 2): unchanged.
4. **P2, sysfs:** `/sys/devices/system/cpu/` as a directory with `possible`, `online` and `cpuN`; `cpufreq`.
   Cosmetic for both translators.
5. Still unknown: whatever FEX hits once it executes x86 code, for example signal frames, `/proc/self/maps`
   accuracy for its allocator, and its 47-bit reservation of the upper half. The size of the committed
   memory outside the arena (`HVF_TABLES` caps it at about 32 GiB in total) may matter for FEX's code cache.
6. Fixed on this branch: ID-register MRS, SIGBUS `BUS_ADRALN`, `PR_SET/GET_MEM_MODEL`,
   `lstat /proc/<pid>/exe` and `MAP_GROWSDOWN`.

### Revised estimate and recommendation

- E2 and plan item 3 are done, and the four small gaps above are fixed: about 1 week less than the plan.
  Abstract sockets add 2–3 days.
- Item 1 (address space) is now merged, including the brk-room fix (7.2), apart from hint placement
  (about 2–3 days).
- 7.1 shows the runtime already runs real x86-64 glibc programs, threads and x86 exec chains through a
  guest translator. So the remaining risk is translator compatibility, not runtime architecture.
- **Revised estimate: about 6–10 weeks to a preview**, down from 2–3 months:
  - abstract sockets (in progress) and hints: under 1 week (the brk room is done, 7.2);
  - binfmt and the hidden mount: 1 week;
  - control plane: 3–5 days;
  - compatibility corpus: 3–6 weeks;
  - packaging: 1–2 weeks.
- The corpus is still the uncertain part. Re-run FEX as soon as the brk-room fix lands to size it.

**Go: FEX as the translator. No-go: Box64 as the default.**
- FEX:
  - It asks the kernel for hardware TSO, which we now provide, and it confirms hardware TSO.
  - It is libc-agnostic: it needs only its own arm64 glibc set, not an x86 one as well.
  - Its official PPA build runs its tools under our runtime. Only abstract AF_UNIX sockets (for
    FEXServer) stand between it and x86 code (7.2).
  - Its remaining needs are generic Linux behaviour, which arm64 workloads also benefit from.
- Box64:
  - It does not use hardware TSO, so it is weak-ordered by default.
  - It needs fixed bridge mappings below 4 GiB.
  - It depends on wrapping a native glibc next to the x86 libraries. 7.1 shows the cost: the image's
    OpenSSL 3.3/3.4 symbols fail against the arm64 root's OpenSSL 3.0, and musl binaries get bound to glibc.
  - It runs real programs today (7.1). It is the right tool for benchmarks and for exercising the runtime
    while FEX is unblocked, but not the default.
- The go for FEX is conditional on abstract sockets; the brk room is done (7.2). Then re-run 7.1 with the
  same `--trace` before plan items 2 and 4.

---

## Key uncertainties

1. ~~Whether HVF lets us set `ACTLR_EL1.TSO` for our vCPUs (E2).~~ Answered (section 7): yes, through the public
   `HV_SYS_REG_ACTLR_EL1` on macOS 15+.
2. Whether FEX can be made self-contained (static or RUNPATH plus PT_INTERP override) without forking it.
   E1 ran the stock PPA build inside an Ubuntu root, so this is still open.
3. How well Box64 handles musl and static x86 binaries. Answered in 7.1: simple commands work, but symbols
   resolve against glibc and static shells crash. Box64 itself warns about static binaries.
4. Any undocumented Rosetta checks beyond the ioctl. This is moot if we follow the recommendation.
5. What FEX needs once it runs x86 code. It is now blocked only by abstract AF_UNIX sockets for FEXServer
   (7.2).

## Sources

- Apple: [VZLinuxRosettaDirectoryShare](https://developer.apple.com/documentation/virtualization/vzlinuxrosettadirectoryshare) ·
  [VZLinuxRosettaUnixSocketCachingOptions](https://developer.apple.com/documentation/virtualization/vzlinuxrosettaunixsocketcachingoptions) ·
  [Accelerating the performance of Rosetta (TSO kernel patch, prctl)](https://developer.apple.com/documentation/virtualization/accelerating-the-performance-of-rosetta) ·
  [hv_sys_reg_t / HV_SYS_REG_ACTLR_EL1 (macOS 15+)](https://developer.apple.com/documentation/hypervisor/hv_sys_reg_t) ·
  [macOS Tahoe 26 SLA](https://www.apple.com/legal/sla/docs/macOSTahoe.pdf) ·
  [Forum: Rosetta vDSO DT_HASH](https://developer.apple.com/forums/thread/769162) ·
  [Forum: no AVX in Rosetta Linux](https://developer.apple.com/forums/thread/769486)
- Reverse engineering and integrations: [threedots.ovh: Quick look at Rosetta on Linux](https://threedots.ovh/blog/2022/06/quick-look-at-rosetta-on-linux/) ·
  [zenn.dev: rosetta + libkrun (ioctls 0x22/0x23)](https://zenn.dev/orimanabu/articles/rosetta-libkrun) ·
  [libkrun PR #176 (Rosetta removed)](https://github.com/containers/libkrun/pull/176) ·
  [UTM Rosetta docs](https://docs.getutm.app/advanced/rosetta/) ·
  [NixOS rosetta.nix](https://raw.githubusercontent.com/NixOS/nixpkgs/master/nixos/modules/virtualisation/rosetta.nix) ·
  [Parallels: Sequoia breaks Rosetta check](https://forum.parallels.com/threads/rosetta-2-no-longer-available-under-macos-15-beta-in-linux-vms.365156/post-933012) ·
  [MacRumors: Rosetta timeline](https://www.macrumors.com/2026/02/16/macos-tahoe-26-4-rosetta-2-warnings/)
- TSO kernel work: [series (VM, macOS 14.4)](https://lkml.iu.edu/hypermail/linux/kernel/2404.1/04656.html) ·
  [PR_SET_MEM_MODEL values](https://lkml.rescloud.iu.edu/2404.1/04826.html) ·
  [ACTLR bit 1 / AIDR bit 9](https://lkml.rescloud.iu.edu/2404.1/04830.html)
- x86-64-v3 breakage: [RHEL-33471](https://issues.redhat.com/browse/RHEL-33471) · [Red Hat KB 7122807](https://access.redhat.com/solutions/7122807)
- FEX: [repo](https://github.com/FEX-Emu/FEX) · [FEX-2207 (FEXServer)](https://newreleases.io/project/github/FEX-Emu/FEX/release/FEX-2207) ·
  [FEX-2306 (HW TSO)](https://fex-emu.com/FEX-2306/) · [FEX-2407 (AVX/AVX2)](https://fex-emu.com/FEX-2407/) ·
  [Fedora FEX change (4K pages, comparison)](https://www.fedoraproject.org/wiki/Changes/FEX) ·
  [ARM64EC / FEXCore as library](https://wiki.fex-emu.com/index.php/Development:ARM64EC)
- Box64: [COMPILE.md (STATICBUILD)](https://github.com/ptitSeb/box64/blob/main/docs/COMPILE.md) ·
  [v0.3.0 (AVX/AVX2)](https://newreleases.io/project/github/ptitSeb/box64/release/v0.3.0) ·
  [benchmarks vs QEMU/FEX/Rosetta](https://box86.org/2022/03/box86-box64-vs-qemu-vs-fex-vs-rosetta2/)
- Blink: [repo/README](https://github.com/jart/blink)
- QEMU: [user-mode docs](https://www.qemu.org/docs/master/user/main.html) · [7.2 release (AVX2 TCG)](https://qemu.org/2022/12/14/qemu-7-2-0)
- Others: [OrbStack docs](https://docs.orbstack.dev/docker/) · [apple/container command reference](https://github.com/apple/container/blob/main/docs/command-reference.md)
