package main

import (
	"os"
	"path/filepath"
	"strconv"
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
	view := guestView{files: layers{upper: root}}
	if p := lookPath(view, []string{"PATH=/usr/bin:/bin"}, "/", "sh"); p != "/bin/sh" {
		t.Fatalf("lookPath sh: %s", p)
	}
	if p := lookPath(view, nil, "/", "/custom/tool"); p != "/custom/tool" {
		t.Fatalf("absolute: %s", p)
	}
	if p := lookPath(view, nil, "/home/runner", "/usr/bin/ls"); p != "/usr/bin/ls" { // not joined to the working directory
		t.Fatalf("absolute with a working directory: %q", p)
	}
	mustReject(t, func() { lookPath(view, nil, "/", "data") }) // not executable
	mustReject(t, func() { lookPath(view, nil, "/", "missing") })
	if p := lookPath(view, nil, "/home/runner", "./bin/tool"); p != "/home/runner/bin/tool" { // relative to the working directory
		t.Fatalf("relative path: %q", p)
	}
	many := make([]string, 100) // act passes more than 64 variables to an action
	for i := range many {
		many[i] = "V" + strconv.Itoa(i) + "=x"
	}
	validateEnv(many)
	values := make([]any, len(many))
	for i, v := range many {
		values[i] = v
	}
	if got := len(stringsOf(values)); got != 100 {
		t.Fatalf("stringsOf: %d", got)
	}
	if got := strings.Join(mergeEnv([]string{"A=1", "B=2"}, []string{"B=3", "C=4"}), " "); got != "A=1 B=3 C=4" {
		t.Fatalf("mergeEnv: %s", got)
	}
}

// docker exec runs a program in a bind mount or volume, by path or found in PATH,
// as the container sees it: the mount hides the files beneath it, its symlinks
// resolve inside the container (never to the Mac's files beside the mount's
// source), and a new volume holds the image's files before the lookup.
func TestExecLookupInMounts(t *testing.T) {
	b := testEngine(t)
	src, outside := t.TempDir(), t.TempDir()
	elf := append([]byte("\x7fELF\x02\x01\x01"), make([]byte, 57)...)
	check(os.WriteFile(filepath.Join(src, "prog"), elf, 0o755))
	check(os.WriteFile(filepath.Join(src, "tool.sh"), []byte("#!/bin/sh\n"), 0o755))
	check(os.WriteFile(filepath.Join(outside, "prog"), elf, 0o755))
	up, e := filepath.Rel(src, filepath.Join(outside, "prog"))
	check(e)
	check(os.Symlink(up, filepath.Join(src, "up")))                              // ../<outside>/prog on the Mac
	check(os.Symlink(filepath.Join(outside, "prog"), filepath.Join(src, "abs"))) // a Mac path the container has not
	check(os.Symlink("/bin/sh", filepath.Join(src, "sh")))                       // the container's /bin/sh
	b.create("box", decode([]byte(`{"Image":"dockiva/shell:demo","HostConfig":{"Binds":["`+src+`:/m"]}}`), maxJSON))
	p := b.container("box")
	c := readObject(filepath.Join(p, "config.json"))
	check(os.MkdirAll(filepath.Join(p, "rootfs", "m"), 0o755))
	check(os.WriteFile(filepath.Join(p, "rootfs", "m", "hidden"), elf, 0o755)) // beneath the mount
	view, env := containerView(p, c), []string{"PATH=/m:/bin"}
	if got := lookPath(view, env, "/", "prog"); got != "/m/prog" {
		t.Fatalf("PATH lookup in a bind mount: %q", got)
	}
	for _, name := range []string{"hidden", "up", "abs"} {
		mustReject(t, func() { lookPath(view, env, "/", name) })
	}
	inside, _ := view.files.find("bin/sh")
	if host, _ := view.find(resolveGuest("m/sh", view.link)); host == "" || host != inside {
		t.Fatalf("a link in a mount left the container: %q", host)
	}
	if cmd, argv0 := containerCommand(p, c, "/", env, []string{"prog", "-v"}); strings.Join(cmd, " ") != "/m/prog -v" || argv0 != "prog" {
		t.Fatalf("exec prog: %v %q", cmd, argv0)
	}
	if cmd, _ := containerCommand(p, c, "/", nil, []string{"/m/tool.sh"}); strings.Join(cmd, " ") != "/bin/sh /m/tool.sh" {
		t.Fatalf("exec /m/tool.sh: %v", cmd)
	}
	if run := runtimeCommand(p, b.Native, c, []string{"/m/prog"}, "/m/prog", env, false); run[2] != filepath.Join(src, "prog") {
		t.Fatalf("runtime program: %v", run[:3])
	}
	if f := b.guestFile("box", "/m/prog"); f.host != filepath.Join(src, "prog") { // docker cp shares the view
		t.Fatalf("docker cp: %q", f.host)
	}
	// A new volume at /bin receives the image's /bin first: its sh, not the image's, runs.
	b.create("vol", decode([]byte(`{"Image":"dockiva/shell:demo","HostConfig":{"Mounts":[{"Type":"volume","Source":"tools","Target":"/bin"}]}}`), maxJSON))
	p = b.container("vol")
	c = readObject(filepath.Join(p, "config.json"))
	if run := runtimeCommand(p, b.Native, c, []string{"/bin/sh"}, "sh", nil, false); run[2] != filepath.Join(volumeData(b.Home, "tools"), "sh") {
		t.Fatalf("runtime program in a volume: %v", run[:3])
	}
}
