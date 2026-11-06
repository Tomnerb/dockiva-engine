package main

import (
	"archive/tar"
	"bytes"
	"io"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

func TestArchiveRoundTripAndImageRemoval(t *testing.T) {
	b := testEngine(t)
	b.create("box", decode([]byte(`{"Image":"dockiva/shell:demo"}`), maxJSON))
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
	mustReject(t, func() { b.removeImage("dockiva/shell:demo", false) }) // box uses it
	if out := b.removeImage("dockiva/shell:demo", true); len(out) != 2 {
		t.Fatalf("rmi: %v", out)
	}
}

// TestDiffLeavesOutFexState: docker diff of an amd64 container lists the program's own
// files, not the config, telemetry and server socket FEX left beside them (x86.go).
func TestDiffLeavesOutFexState(t *testing.T) {
	b := &engine{State: t.TempDir()}
	lower, p := t.TempDir(), filepath.Join(b.State, "x86")
	upper := filepath.Join(p, "rootfs")
	write := func(root, rel, content string) {
		check(os.MkdirAll(filepath.Dir(filepath.Join(root, rel)), 0o755))
		check(os.WriteFile(filepath.Join(root, rel), []byte(content), 0o644))
	}
	write(lower, "etc/passwd", "root:x:0:0:root:/root:/bin/sh\napp:x:1000:1000::/home/app:/bin/sh\n")
	for _, dir := range []string{"root", "home/app", "tmp"} {
		check(os.MkdirAll(filepath.Join(lower, dir), 0o755))
	}
	for _, rel := range []string{"hello", "root/.config/myapp/conf", "root/.config/fex-emu/AppConfig/sh.json",
		"root/.local/share/fex-emu/Server/Server.lock", "root/.local/share/fex-emu/Telemetry/sh.telem", "home/app/.config/fex-emu/AppConfig/sh.json"} {
		write(upper, rel, "x")
	}
	// FEXServer's socket: bound where its path fits in a sockaddr, then moved into /tmp.
	short, e := os.MkdirTemp("/tmp", "fex")
	check(e)
	defer os.RemoveAll(short)
	fd, e := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	check(e)
	defer syscall.Close(fd)
	check(syscall.Bind(fd, &syscall.SockaddrUnix{Name: filepath.Join(short, "s")}))
	check(os.MkdirAll(filepath.Join(upper, "tmp"), 0o755))
	check(os.Rename(filepath.Join(short, "s"), filepath.Join(upper, "tmp", "0"+fexSocket)))
	diff := func(arch string) string {
		save(filepath.Join(p, "config.json"), obj{"lower": lower, "arch": arch})
		save(filepath.Join(p, "status.json"), obj{"state": "exited"})
		lines := []string{}
		for _, change := range b.changes("x86") {
			lines = append(lines, map[int]string{0: "C", 1: "A", 2: "D"}[change["Kind"].(int)]+" "+str(change["Path"]))
		}
		return strings.Join(lines, ", ")
	}
	if got, want := diff("amd64"), "A /hello, C /root, A /root/.config, A /root/.config/myapp, A /root/.config/myapp/conf"; got != want {
		t.Fatalf("amd64 diff:\n got %s\nwant %s", got, want)
	}
	got := diff("arm64") // FEX runs only amd64 programs: an arm64 container's files of these names are its own
	for _, want := range []string{"A /tmp/0.FEXServer.Socket", "A /home/app/.config/fex-emu/AppConfig/sh.json", "A /root/.local/share/fex-emu/Telemetry/sh.telem"} {
		if !strings.Contains(got, want) {
			t.Fatalf("arm64 diff lacks %s: %s", want, got)
		}
	}
}
