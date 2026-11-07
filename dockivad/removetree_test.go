package main

import (
	"os"
	"path/filepath"
	"testing"
)

// A container's Go module cache has 0555 directories; removing it must still work,
// and must not follow a symlink out of the tree.
func TestRemoveTreeReadOnlyDirs(t *testing.T) {
	outside := filepath.Join(t.TempDir(), "outside")
	check(os.MkdirAll(outside, 0o500))
	p := filepath.Join(t.TempDir(), "gone")
	dir := filepath.Join(p, "go/pkg/mod/x@v1")
	check(os.MkdirAll(dir, 0o755))
	check(os.WriteFile(filepath.Join(dir, "a.go"), nil, 0o444))
	check(os.Symlink(outside, filepath.Join(dir, "link")))
	for _, d := range []string{dir, filepath.Dir(dir)} {
		check(os.Chmod(d, 0o555))
	}
	if e := removeTree(p); e != nil {
		t.Fatal(e)
	}
	if _, e := os.Lstat(p); !os.IsNotExist(e) {
		t.Fatalf("still there: %v", e)
	}
	if st, e := os.Stat(outside); e != nil || st.Mode().Perm() != 0o500 {
		t.Fatalf("symlink target changed: %v %v", st.Mode(), e)
	}
}
