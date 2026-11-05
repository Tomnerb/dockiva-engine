package main

// linux/amd64 images run through FEX, an x86-64 translator that is itself an arm64
// Linux program: the runtime acts as binfmt_misc with FEX registered (its
// --x86-translator; native/fs.h x86_exec_args), and FEX comes from a bundle (bin/FEX,
// bin/FEXServer, lib/ with their arm64 glibc) mounted read-only into each amd64
// container at x86Guest. A pull prefers linux/arm64 and takes linux/amd64 when an
// image has no arm64 variant, or when asked (--platform linux/amd64), as Docker on
// Apple silicon does.

import (
	"os"
	"path/filepath"
	"strings"
)

const x86Guest = "/.dockiva/x86"

// platformArch is a --platform value's architecture: "" when none is given.
func platformArch(platform string) string {
	switch strings.TrimSpace(strings.ToLower(platform)) {
	case "":
		return ""
	case "linux/arm64", "linux/arm64/v8", "arm64", "linux/aarch64":
		return "arm64"
	case "linux/amd64", "amd64", "linux/x86_64", "x86_64":
		return "amd64"
	}
	panic(apiError{400, "Only linux/arm64 and linux/amd64 (through FEX) are supported, not " + platform})
}

// imageArch is an image's architecture ("arm64" when its config names none).
func imageArch(im *image) string {
	if im.Arch == "" {
		return "arm64"
	}
	return im.Arch
}

// x86Bundle is FEX's directory on the Mac: $DOCKIVA_X86, beside the runtime (an
// app's Resources/x86), or <home>/x86; "" when none holds bin/FEX.
func x86Bundle(native, home string) string {
	for _, dir := range []string{os.Getenv("DOCKIVA_X86"), filepath.Join(filepath.Dir(native), "x86"), filepath.Join(home, "x86")} {
		if dir == "" {
			continue
		}
		if st, e := os.Stat(filepath.Join(dir, "bin", "FEX")); e == nil && st.Mode().IsRegular() {
			return dir
		}
	}
	return ""
}

// x86Args are the runtime's options for an amd64 container: FEX mounted read-only
// and named as the translator.
func x86Args(r layers, native, home string) []string {
	bundle := x86Bundle(native, home)
	reject(bundle != "", "linux/amd64 images run through FEX, which is not installed: put its bundle (bin/FEX, bin/FEXServer, lib/) in "+filepath.Join(home, "x86")+" or set DOCKIVA_X86")
	_ = safeRun(func() { r.mountPoint(strings.TrimPrefix(x86Guest, "/"), true) })
	return []string{"--bind-ro", bundle, x86Guest, "--x86-translator", x86Guest}
}
