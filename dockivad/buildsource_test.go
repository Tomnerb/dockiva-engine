package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/klauspost/compress/zstd"
)

func TestIsGitContext(t *testing.T) {
	for s, want := range map[string]bool{
		"https://github.com/docker/cli.git":            true,
		"https://github.com/docker/cli.git#v29.0:docs": true,
		"git://example.com/repo":                       true,
		"git@github.com:docker/cli.git":                true,
		"ssh://git@example.com/repo.git":               true,
		"github.com/docker/cli":                        true,
		"https://example.com/context.tar.gz":           false,
		"https://example.com/Dockerfile":               false,
		"./app":                                        false,
		"/Users/me/app.git":                            false,
	} {
		if got := isGitContext(s); got != want {
			t.Errorf("isGitContext(%q) = %v, want %v", s, got, want)
		}
	}
}

// A tar archive in each compression ADD unpacks, and compressed files that are not
// archives (copied, as Docker does).
func TestArchiveStream(t *testing.T) {
	dir := t.TempDir()
	var plain bytes.Buffer
	tw := tar.NewWriter(&plain)
	if e := tw.WriteHeader(&tar.Header{Name: "hello.txt", Mode: 0o644, Size: 3, Typeflag: tar.TypeReg}); e != nil {
		t.Fatal(e)
	}
	tw.Write([]byte("hi\n"))
	tw.Close()
	write := func(name string, data []byte) string {
		p := filepath.Join(dir, name)
		if e := os.WriteFile(p, data, 0o600); e != nil {
			t.Fatal(e)
		}
		return p
	}
	var gz bytes.Buffer
	g := gzip.NewWriter(&gz)
	g.Write(plain.Bytes())
	g.Close()
	var zs bytes.Buffer
	z, _ := zstd.NewWriter(&zs)
	z.Write(plain.Bytes())
	z.Close()
	var notTar bytes.Buffer
	g = gzip.NewWriter(&notTar)
	g.Write([]byte("just text, not a tar archive\n"))
	g.Close()
	cases := map[string]bool{
		write("a.tar", plain.Bytes()):      true,
		write("a.tar.gz", gz.Bytes()):      true,
		write("a.tar.zst", zs.Bytes()):     true,
		write("text.gz", notTar.Bytes()):   false,
		write("text.txt", []byte("hello")): false,
		write("empty", nil):                false,
	}
	mk := exec.Command("/usr/bin/tar", "-cJf", "-", "-C", dir, "a.tar") // a .tar.xz of a tar: unpacks to a.tar
	mk.Env = append(os.Environ(), "COPYFILE_DISABLE=1")                 // no AppleDouble ._ members
	if xz, e := mk.Output(); e == nil {
		cases[write("a.tar.xz", xz)] = true
	}
	for p, want := range cases {
		r, ok := archiveStream(p)
		if ok != want {
			t.Fatalf("%s: archive %v, want %v", filepath.Base(p), ok, want)
		}
		if !ok {
			continue
		}
		h, e := tar.NewReader(r).Next()
		r.Close()
		if e != nil || (h.Name != "hello.txt" && h.Name != "a.tar") {
			t.Fatalf("%s: first member %v %v", filepath.Base(p), h, e)
		}
	}
}

// RUN --mount=type=ssh: numbered targets, and the first one is SSH_AUTH_SOCK.
func TestSSHMountTargets(t *testing.T) {
	bd := &builder{escape: '\\'}
	st := &buildStage{config: obj{}}
	in, _ := parseDockerfile("RUN --mount=type=ssh --mount=type=ssh,id=work --mount=type=ssh,target=/s.sock true\n")
	o := bd.runOptions(st, in[0], map[string]string{})
	got := map[string]string{}
	for _, m := range o.mounts {
		got[m.target] = m.id + "," + m.env
	}
	want := map[string]string{"/run/buildkit/ssh_agent.0": "default,SSH_AUTH_SOCK", "/run/buildkit/ssh_agent.1": "work,", "/s.sock": "default,"}
	if len(got) != len(want) {
		t.Fatalf("mounts %v", got)
	}
	for k, v := range want {
		if got[k] != v {
			t.Fatalf("mount %s: %q, want %q (all %v)", k, got[k], v, got)
		}
	}
}
