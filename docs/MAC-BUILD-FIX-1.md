# macOS build fix 1

Enable _DARWIN_C_SOURCE before all headers on Apple platforms, while retaining
_POSIX_C_SOURCE=200809L. The strict POSIX setting added in M7 hid INADDR_LOOPBACK
and O_NOFOLLOW in the user's macOS SDK. No filesystem checks are removed and no
platform flag values are invented.

The Linux build and all 40 combined checks passed after this change. Compilation
and execution on macOS still require user validation. Rebuild using:

    python3 scripts/build_native.py
    python3 scripts/verify_m7_10.py --report mac-m7-10-validation.json
