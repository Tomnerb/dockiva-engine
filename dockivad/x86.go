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
	"path"
	"path/filepath"
	"slices"
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

// fexState lists what FEX makes under a home as x86 programs run (.config/fex-emu:
// its config; .local/share/fex-emu: FEXServer's lock and telemetry) with their
// parents, children first, for each home a RUN step's programs may have: "/", /root,
// the stage's HOME and /etc/passwd's. Those missing before an amd64 step are gone
// after it (removeFexState), so its layer holds the step's own files only.
// ponytail: a step that sets XDG_CONFIG_HOME or XDG_DATA_HOME keeps FEX's files there.
func fexState(root string, env []string) []string {
	homes := []string{"/", "/root"}
	for _, kv := range env {
		if h, ok := strings.CutPrefix(kv, "HOME="); ok {
			homes = append(homes, h)
		}
	}
	for _, f := range passwdEntries(layers{upper: root}, filepath.Join("etc", "passwd")) {
		if len(f) >= 6 {
			homes = append(homes, f[5])
		}
	}
	out := []string{}
	for _, h := range homes {
		for _, p := range []string{".config/fex-emu", ".local/share/fex-emu", ".config", ".local/share", ".local"} {
			if p = path.Join(h, p); strings.HasPrefix(h, "/") && !slices.Contains(out, p) {
				out = append(out, p)
			}
		}
	}
	return out
}

// missingIn is the guest paths not in root (one that does not resolve counts as there).
func missingIn(root string, guest []string) []string {
	out := []string{}
	for _, g := range guest {
		_ = safeRun(func() {
			if _, e := os.Lstat(filepath.Join(root, guestRel(root, g))); e != nil {
				out = append(out, g)
			}
		})
	}
	return out
}

// removeFexState removes what fexState listed that a step made: FEX's directories
// with what is in them, the others when empty (the step's own files keep them),
// and FEXServer's sockets (/tmp/<uid>.FEXServer.Socket), left as its process ends.
func removeFexState(root string, made []string) {
	_ = safeRun(func() {
		sockets, _ := filepath.Glob(filepath.Join(noLinks(root, "tmp"), "*.FEXServer.Socket"))
		for _, s := range sockets {
			if st, e := os.Lstat(s); e == nil && st.Mode()&os.ModeSocket != 0 {
				_ = os.Remove(s)
			}
		}
	})
	for _, g := range made {
		_ = safeRun(func() {
			rel := guestRel(root, g)
			host := filepath.Join(noLinks(root, path.Dir(rel)), path.Base(rel))
			if st, e := os.Lstat(host); e != nil || !st.IsDir() { // a file the step made there stays
				return
			}
			if path.Base(rel) == "fex-emu" {
				check(os.RemoveAll(host))
			} else {
				_ = os.Remove(host) // a directory still holding files stays
			}
		})
	}
}

// x86Args are the runtime's options for an amd64 container: FEX mounted read-only
// and named as the translator.
func x86Args(r layers, native, home string) []string {
	bundle := x86Bundle(native, home)
	reject(bundle != "", "linux/amd64 images run through FEX, which is not installed: put its bundle (bin/FEX, bin/FEXServer, lib/) in "+filepath.Join(home, "x86")+" or set DOCKIVA_X86")
	_ = safeRun(func() { r.mountPoint(strings.TrimPrefix(x86Guest, "/"), true) })
	return []string{"--bind-ro", bundle, x86Guest, "--x86-translator", x86Guest}
}
