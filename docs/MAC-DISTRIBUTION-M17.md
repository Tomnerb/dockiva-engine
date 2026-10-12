# M17 macOS distribution tooling

This source release includes a Mac packaging script, a versioned user-space
installer, a rollback-friendly `current` symlink, and `vmfree.py doctor`.
It does not include a signed/notarized Mac executable built in this Linux session.
The signed-install acceptance gate remains OPEN.

## Build on the Mac

```sh
python3 scripts/package_macos.py --output dist/vmfree-macos.zip
```

This builds and verifies the runtime, then uses `ditto` to create a distribution
with the Mac executable and Python entrypoints. The end user needs Python 3.10+;
a Python runtime is not bundled. This therefore does not yet meet a clean-Mac,
zero-dependency distribution goal. No GUI or automatic updater is included.

To sign and submit, use your own Developer ID Application identity and an
already configured notarytool keychain profile:

```sh
python3 scripts/package_macos.py \
  --identity 'Developer ID Application: YOUR NAME (TEAMID)' \
  --notary-profile YOUR_EXISTING_PROFILE \
  --output dist/vmfree-macos.zip
```

The script signs/verifies the native executable with hardened runtime and submits
the ZIP to Apple's notarization service. It does not create or transmit credentials
in source files. A ZIP cannot be stapled; a future app/DMG/PKG packaging flow is
needed for a stapled offline distribution. Signing the native executable alone
does not authenticate editable Python sources as an application bundle.

After extracting that Mac-built ZIP:

```sh
python3 vmfree/install.py
python3 ~/.local/share/vmfree/current/vmfree.py doctor
python3 ~/.local/share/vmfree/current/vmfree.py engine
```

The installer uses `~/.local/share/vmfree/0.17.0` and selects it through `current`.
It does not overwrite the same version, change your Docker context, install a
login item, or remove runtime state in `~/.vmfree`. Future versions can be installed
in separate version directories. To roll back, stop the engine and select a known
previous installation with a replacement `current` symlink; no automated rollback
command is supplied yet.

Updates are manual versioned installations. There is no update feed, automatic
signature verification of a feed, or automatic crash/reboot recovery. Uninstalling
the CLI directory does not delete your container state. Stop guests before removing
an installation containing their interpreter.

Validate the package on a clean Apple Silicon Mac, check Gatekeeper behavior,
notary status, install/start/stop/update/rollback and removal, and separately decide
how to bundle Python before calling M17 complete.

[Apple notarization documentation](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)
