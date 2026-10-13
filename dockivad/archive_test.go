package main

import (
	"archive/tar"
	"bytes"
	"io"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestArchiveRoundTripAndImageRemoval(t *testing.T) {
	b := testEngine(t)
	b.create("box", decode([]byte(`{"Image":"vmfree/shell:demo"}`), maxJSON))
	var in bytes.Buffer
	w := tar.NewWriter(&in)
	for _, h := range []*tar.Header{{Name: "d/", Typeflag: tar.TypeDir, Mode: 0o755}, {Name: "d/f", Typeflag: tar.TypeReg, Mode: 0o644, Size: 2}, {Name: "d/l", Typeflag: tar.TypeSymlink, Linkname: "f"}} {
		check(w.WriteHeader(h))
		if h.Size > 0 {
			_, _ = w.Write([]byte("hi"))
		}
	}
	check(w.Close())
	b.putArchive("box", "/", &in, false)
	for _, bad := range []string{"../x", "/etc/../../x"} {
		var t2 bytes.Buffer
		w := tar.NewWriter(&t2)
		check(w.WriteHeader(&tar.Header{Name: bad, Typeflag: tar.TypeReg, Size: 0}))
		check(w.Close())
		mustReject(t, func() { b.putArchive("box", "/", &t2, false) })
	}
	// A symlink in the container never leads the copy out of its root.
	check(os.Symlink("/", filepath.Join(b.State, "box", "rootfs", "escape")))
	if f := b.guestFile("box", "/escape/d/f"); f.host != filepath.Join(b.State, "box", "rootfs", "d", "f") {
		t.Fatalf("symlink resolved outside the container: %s", f.host)
	}
	r := httptest.NewRecorder()
	b.getArchive(r, "box", "/d/.", false)
	names := []string{}
	for out := tar.NewReader(r.Body); ; {
		h, e := out.Next()
		if e == io.EOF {
			break
		}
		check(e)
		names = append(names, h.Name)
	}
	if len(names) != 3 || names[0] != "./" || names[1] != "./f" || names[2] != "./l" {
		t.Fatalf("archive: %v", names)
	}
	mustReject(t, func() { b.removeImage("vmfree/shell:demo", false) }) // box uses it
	if out := b.removeImage("vmfree/shell:demo", true); len(out) != 2 {
		t.Fatalf("rmi: %v", out)
	}
}
