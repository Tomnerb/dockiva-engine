package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestExecHelpers(t *testing.T) {
	root := t.TempDir()
	for _, d := range []string{"bin", "usr/bin"} {
		check(os.MkdirAll(filepath.Join(root, d), 0o755))
	}
	check(os.WriteFile(filepath.Join(root, "bin/busybox"), nil, 0o755))
	check(os.WriteFile(filepath.Join(root, "usr/bin/data"), nil, 0o644))
	check(os.Symlink("/bin/busybox", filepath.Join(root, "bin/sh")))
	if p := lookPath(layers{upper: root}, []string{"PATH=/usr/bin:/bin"}, "/", "sh"); p != "/bin/sh" {
		t.Fatalf("lookPath sh: %s", p)
	}
	if p := lookPath(layers{upper: root}, nil, "/", "/custom/tool"); p != "/custom/tool" {
		t.Fatalf("absolute: %s", p)
	}
	mustReject(t, func() { lookPath(layers{upper: root}, nil, "/", "data") }) // not executable
	mustReject(t, func() { lookPath(layers{upper: root}, nil, "/", "missing") })
	if p := lookPath(layers{upper: root}, nil, "/home/runner", "./bin/tool"); p != "/home/runner/bin/tool" { // relative to the working directory
		t.Fatalf("relative path: %q", p)
	}
	if got := strings.Join(mergeEnv([]string{"A=1", "B=2"}, []string{"B=3", "C=4"}), " "); got != "A=1 B=3 C=4" {
		t.Fatalf("mergeEnv: %s", got)
	}
}
