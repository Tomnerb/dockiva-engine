package main

import (
	"archive/tar"
	"bytes"
	"fmt"
	"io"
	"io/fs"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// Guest trees with directories the engine's user may not change or read: 0555 (Go's
// module cache; guest root's chmod) and 000, each holding files.

// restrictedTree makes, in dir (made if missing):
//
//	m/a/f      m and a 0555
//	z/k/f      z 000, k 0555
//	s          a 000 file
func restrictedTree(t *testing.T, dir string) {
	t.Helper()
	for _, d := range []string{"m/a", "z/k"} {
		check(os.MkdirAll(filepath.Join(dir, d), 0o755))
		check(os.WriteFile(filepath.Join(dir, d, "f"), []byte(d), 0o644))
	}
	check(os.WriteFile(filepath.Join(dir, "s"), []byte("secret"), 0o600))
	for _, rel := range []string{"m/a", "m", "z/k"} { // children first
		check(os.Chmod(filepath.Join(dir, rel), 0o555))
	}
	for _, rel := range []string{"z", "s"} {
		check(os.Chmod(filepath.Join(dir, rel), 0))
	}
}

// unlockLater lets the test's temporary directory go: t.TempDir's own removal stops
// at such directories.
func unlockLater(t *testing.T, dir string) {
	t.Cleanup(func() {
		_ = filepath.WalkDir(dir, func(p string, d fs.DirEntry, e error) error {
			if e == nil && d.IsDir() {
				_ = os.Chmod(p, 0o700)
			}
			return nil
		})
	})
}

// treeModes lists a tree as "rel mode [content]" lines, reading into directories
// without permission (their modes are put back).
func treeModes(t *testing.T, root string) string {
	t.Helper()
	lines := []string{}
	var visit func(rel string)
	visit = func(rel string) {
		p := filepath.Join(root, rel)
		st, e := os.Lstat(p)
		check(e)
		line := fmt.Sprintf("%s %v", rel, st.Mode())
		switch {
		case st.Mode().IsRegular():
			check(os.Chmod(p, 0o600))
			data, e := os.ReadFile(p)
			check(e)
			check(os.Chmod(p, st.Mode().Perm()))
			line += " " + string(data)
		case st.IsDir():
			check(os.Chmod(p, 0o700))
			entries, e := os.ReadDir(p)
			check(e)
			for _, entry := range entries {
				visit(filepath.Join(rel, entry.Name()))
			}
			check(os.Chmod(p, st.Mode().Perm()))
		}
		lines = append(lines, line)
	}
	visit(".")
	sort.Strings(lines)
	return strings.Join(lines, "\n")
}

const restrictedModes = `. drwxr-xr-x
m dr-xr-xr-x
m/a dr-xr-xr-x
m/a/f -rw-r--r-- m/a
s ---------- secret
z d---------
z/k dr-xr-xr-x
z/k/f -rw-r--r-- z/k`

// docker rmi of an image holding such directories removes all of it, and a cache
// directory an earlier engine left half removed (no image.json) breaks nothing.
func TestRemoveImageRestrictedDirs(t *testing.T) {
	b := testEngine(t)
	unlockLater(t, b.Home)
	h := b.socketHandler()
	dir := b.imagePath("dockiva/shell:demo")
	restrictedTree(t, filepath.Join(dir, "rootfs", "data"))
	half := filepath.Join(b.Cache, strings.Repeat("0", 64))
	restrictedTree(t, filepath.Join(half, "rootfs"))
	if r := request(t, h, "GET", "/images/json", "", ""); r.Code != 200 || !strings.Contains(r.Body.String(), "dockiva/shell:demo") {
		t.Fatalf("images beside a half-removed one: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "DELETE", "/images/dockiva/shell:demo", "", ""); r.Code != 200 || !strings.Contains(r.Body.String(), "Deleted") {
		t.Fatalf("rmi: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "GET", "/images/json", "", ""); r.Code != 200 || strings.TrimSpace(r.Body.String()) != "[]" {
		t.Fatalf("images after rmi: %d %s", r.Code, r.Body.String())
	}
	cleanupUnpublished(b.Cache)
	migrateNames(b.Cache) // the next start: the half-removed image goes
	if entries, _ := os.ReadDir(b.Cache); len(entries) != 0 {
		t.Fatalf("left in the image cache: %v", entries)
	}
}

// docker commit's copy: the image's files with the container's laid over them. Both
// hold such directories; the copy has their modes and files, the sources keep theirs,
// and the layer written from it lists them.
func TestCommitCopyRestrictedDirs(t *testing.T) {
	dir := t.TempDir()
	unlockLater(t, dir)
	lower, upper, merged, blobs := filepath.Join(dir, "lower"), filepath.Join(dir, "upper"), filepath.Join(dir, "work", "rootfs"), filepath.Join(dir, "blobs")
	for _, d := range []string{lower, filepath.Join(upper, "m/a"), filepath.Join(upper, "root"), filepath.Dir(merged), blobs} {
		check(os.MkdirAll(d, 0o755))
	}
	restrictedTree(t, lower)
	// The container: a file added to, and one removed from, the image's 0555 and 000
	// directories, and directories of its own.
	check(os.WriteFile(filepath.Join(upper, "m/a/new"), []byte("new"), 0o644))
	check(os.WriteFile(filepath.Join(upper, "m/a", whiteoutPrefix+"f"), nil, 0o600))
	check(os.MkdirAll(filepath.Join(upper, "z"), 0o755))
	check(os.WriteFile(filepath.Join(upper, "z", whiteoutPrefix+"k"), nil, 0o600))
	for _, rel := range []string{"m/a", "m"} {
		check(os.Chmod(filepath.Join(upper, rel), 0o555))
	}
	check(os.Chmod(filepath.Join(upper, "z"), 0))
	restrictedTree(t, filepath.Join(upper, "root"))
	before := treeModes(t, lower) + "\n\n" + treeModes(t, upper)

	cloneTree(lower, merged)
	applyUpper(upper, merged)

	own := strings.ReplaceAll(strings.Replace(restrictedModes, ". drwxr-xr-x\n", "", 1), "\n", "\nroot/")
	want := ". drwxr-xr-x\nm dr-xr-xr-x\nm/a dr-xr-xr-x\nm/a/new -rw-r--r-- new\nroot drwxr-xr-x\nroot/" + own + "\ns ---------- secret\nz d---------"
	if got := treeModes(t, merged); got != want {
		t.Fatalf("merged:\n%s\nwant:\n%s", got, want)
	}
	if after := treeModes(t, lower) + "\n\n" + treeModes(t, upper); after != before {
		t.Fatalf("the sources changed:\n%s\nwere:\n%s", after, before)
	}
	digest, size, changed := writeDiffLayer(merged, lower, blobs)
	_, headers := layerHeaders(t, blobs, digest, size)
	got := []string{}
	for _, h := range headers {
		got = append(got, fmt.Sprintf("%s %o", h.Name, h.Mode))
	}
	sort.Strings(got)
	wantLayer := "m/a/.wh.f 0, m/a/new 644, root/ 755, root/m/ 555, root/m/a/ 555, root/m/a/f 644, root/s 0, root/z/ 0, root/z/k/ 555, root/z/k/f 644, z/.wh.k 0"
	if !changed || strings.Join(got, ", ") != wantLayer {
		t.Fatalf("layer:\n%s\nwant:\n%s", strings.Join(got, ", "), wantLayer)
	}
	if size := treeSize(merged); size != int64(len("new")+2*len("secret")+2*len("m/a")) {
		t.Fatalf("size: %d", size)
	}
	if e := removeTree(filepath.Dir(merged)); e != nil {
		t.Fatal(e)
	}
}

// docker cp and docker export read such directories and files, with their modes,
// and docker cp into a 0555 directory works as it does for Docker's root.
func TestArchiveRestrictedDirs(t *testing.T) {
	b := testEngine(t)
	unlockLater(t, b.Home)
	b.create("box", decode([]byte(`{"Image":"dockiva/shell:demo"}`), maxJSON))
	upper := filepath.Join(b.State, "box", "rootfs")
	restrictedTree(t, filepath.Join(upper, "data"))
	before := treeModes(t, filepath.Join(upper, "data"))
	entries := func(r *httptest.ResponseRecorder, prefix string) string {
		t.Helper()
		lines := []string{}
		for in := tar.NewReader(r.Body); ; {
			h, e := in.Next()
			if e == io.EOF {
				break
			}
			if e != nil {
				t.Fatalf("archive cut short after %v: %v", lines, e)
			}
			if name, ok := strings.CutPrefix(h.Name, prefix); ok {
				data, e := io.ReadAll(in)
				check(e)
				lines = append(lines, strings.TrimSpace(fmt.Sprintf("%s %o %s", name, h.Mode, data)))
			}
		}
		return strings.Join(lines, ", ")
	}
	want := "/ 755, /m/ 555, /m/a/ 555, /m/a/f 644 m/a, /s 0 secret, /z/ 0, /z/k/ 555, /z/k/f 644 z/k"
	r := httptest.NewRecorder()
	b.getArchive(r, "box", "/data", false)
	if got := entries(r, "data"); got != want {
		t.Fatalf("docker cp:\n%s\nwant:\n%s", got, want)
	}
	r = httptest.NewRecorder()
	b.export(r, "box")
	if got := entries(r, "data"); got != want {
		t.Fatalf("docker export:\n%s", got)
	}
	if after := treeModes(t, filepath.Join(upper, "data")); after != before {
		t.Fatalf("the container's files changed:\n%s\nwere:\n%s", after, before)
	}

	// Into the container: a file beside m/a/f, a directory replacing it, and a file in
	// a directory of the image the container has no copy of yet.
	image := filepath.Join(b.imagePath("dockiva/shell:demo"), "rootfs")
	check(os.Mkdir(filepath.Join(image, "ro"), 0o555))
	var in bytes.Buffer
	w := tar.NewWriter(&in)
	for _, h := range []*tar.Header{{Name: "data/m/a/new", Typeflag: tar.TypeReg, Mode: 0o644, Size: 2}, {Name: "data/m/a/f/", Typeflag: tar.TypeDir, Mode: 0o555},
		{Name: "data/m/a/f/in", Typeflag: tar.TypeReg, Mode: 0o600, Size: 2}, {Name: "ro/x", Typeflag: tar.TypeReg, Mode: 0o644, Size: 2}} {
		check(w.WriteHeader(h))
		if h.Size > 0 {
			_, _ = w.Write([]byte("hi"))
		}
	}
	check(w.Close())
	b.putArchive("box", "/", &in, false)
	want = strings.Replace(restrictedModes, "m/a/f -rw-r--r-- m/a", "m/a/f dr-xr-xr-x\nm/a/f/in -rw------- hi\nm/a/new -rw-r--r-- hi", 1)
	if got := treeModes(t, filepath.Join(upper, "data")); got != want {
		t.Fatalf("after docker cp into it:\n%s\nwant:\n%s", got, want)
	}
	if got := treeModes(t, filepath.Join(upper, "ro")); got != ". dr-xr-xr-x\nx -rw-r--r-- hi" {
		t.Fatalf("copied-up directory:\n%s", got)
	}
	b.remove("box")
	if exists(filepath.Join(b.State, "box")) {
		t.Fatal("the container's files stay")
	}
}
