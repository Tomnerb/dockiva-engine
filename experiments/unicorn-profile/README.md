# Local Unicorn memory-path experiment

This is an isolated Apple Silicon experiment, not a runtime dependency. Requires
the prior adapter's generated kernels, clang, codesign, LLVM objcopy, and the
isolated `build/feasibility/venv`. CMake 4.1.3 was installed in that venv.

The downloaded archive is `build/unicorn-profile/unicorn-2.1.4.tar.gz`. Its URL
and SHA-256 are in `evidence/unicorn-profile/source.json`; verify the hash before
extracting. It came from the official unicorn-engine 2.1.4 tag. Keep upstream
license files intact. No distribution licensing decision is made here.

From the repository root, to recreate the two builds from that archive:

```sh
python3 experiments/unicorn-adapter/generate.py
tar -xzf build/unicorn-profile/unicorn-2.1.4.tar.gz -C build/unicorn-profile
cp -R build/unicorn-profile/unicorn-2.1.4 build/unicorn-profile/unicorn-patched
patch -d build/unicorn-profile/unicorn-patched -p1 < experiments/unicorn-profile/nonexec-dirty.patch
build/feasibility/venv/bin/cmake -S build/unicorn-profile/unicorn-2.1.4 -B build/unicorn-profile/baseline -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUNICORN_ARCH=aarch64 -DBUILD_SHARED_LIBS=OFF -DUNICORN_BUILD_TESTS=ON
build/feasibility/venv/bin/cmake --build build/unicorn-profile/baseline -j 6
build/feasibility/venv/bin/cmake -S build/unicorn-profile/unicorn-patched -B build/unicorn-profile/patched -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUNICORN_ARCH=aarch64 -DBUILD_SHARED_LIBS=OFF -DUNICORN_BUILD_TESTS=ON
build/feasibility/venv/bin/cmake --build build/unicorn-profile/patched -j 6
python3 experiments/unicorn-profile/validate.py
```

The copy/patch steps assume `unicorn-patched` does not already exist. Existing
builds from this experiment can directly use `validate.py`. JIT execution may
require authorization outside the tool sandbox. The validation script checks
the applicable `test_arm64` suite; generic upstream tests also require x86 guest
support and must not be treated as passing in this ARM64-only configuration.

`profile.c` runs the same memory kernel for approximately eight seconds. It was
compiled with the same include/native assembly/static library arguments as the
comparison helper, with `-O2 -g`, then sampled using `sample PID 5 1 -file PATH`.
The profile captures ran separately from the final timing comparison. Source
build profiling includes symbols; original-wheel profiling has fewer symbols.
