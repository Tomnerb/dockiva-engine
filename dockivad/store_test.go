package main

import (
	"archive/tar"
	"encoding/json"
	"errors"
	"io/fs"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"syscall"
	"testing"
)

// testStore attaches a new store in a temporary engine home, detached after the test.
func testStore(t *testing.T) (home, store string) {
	t.Helper()
	home = t.TempDir()
	check(os.Chmod(home, 0o700))
	store = openStore(home)
	t.Cleanup(func() { _ = exec.Command("/usr/bin/hdiutil", "detach", "-force", filepath.Join(home, "store")).Run() })
	return home, store
}

// Names that differ only by case are two names on the store: containers, and the
// files of an image's layer. Volumes stay on the Mac's disk, where they are refused.
func TestStoreKeepsCase(t *testing.T) {
	home, store := testStore(t)
	if store != filepath.Join(home, "store") || !mounted(store) || engineHome(filepath.Join(store, "containers", "web")) != home {
		t.Fatalf("store %s mounted %v", store, mounted(store))
	}
	if again := openStore(home); again != store { // attached already: kept
		t.Fatalf("second open: %s", again)
	}
	if exec.Command("/usr/bin/xattr", "-p", "com.apple.metadata:com_apple_backup_excludeItem", filepath.Join(home, storeBundle)).Run() != nil {
		t.Fatal("Time Machine backs the store up")
	}
	b := engineAt(home, store)
	h := b.socketHandler()
	ids := map[string]string{}
	for _, name := range []string{"Web", "web"} {
		if r := request(t, h, "POST", "/containers/create?name="+name, `{"Image":"dockiva/shell:demo","Cmd":["-c","true"]}`, ""); r.Code != 201 {
			t.Fatalf("create %s: %d %s", name, r.Code, r.Body.String())
		}
		ids[name] = str(decodeBody(t, request(t, h, "GET", "/containers/"+name+"/json", "", "").Body.String())["Id"])
	}
	if ids["Web"] == ids["web"] {
		t.Fatal("Web and web are one container")
	}
	if r := request(t, h, "DELETE", "/containers/web", "", ""); r.Code != 204 {
		t.Fatalf("rm web: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "GET", "/containers/Web/json", "", ""); r.Code != 200 {
		t.Fatalf("rm web took Web: %d", r.Code)
	}

	loadCasePair(t, b)
	root := b.image("test/case:1").Root
	checkCasePair(t, root)

	// Detached at shutdown, its free space given back, and attached again with what it held.
	check(os.WriteFile(filepath.Join(store, "big"), make([]byte, 64<<20), 0o600))
	syncDir(store)
	check(os.Remove(filepath.Join(store, "big")))
	before := bundleBytes(t, home)
	closeStore(home)
	if mounted(store) {
		t.Fatal("still attached")
	}
	if after := bundleBytes(t, home); after > before-48<<20 {
		t.Fatalf("compact gave back %d of 64 MiB", (before-after)>>20)
	}
	if openStore(home) != store || !exists(filepath.Join(root, "netfilter", "xt_connmark.h")) {
		t.Fatal("reattached without its files")
	}
}

// bundleBytes is the disk space the store's bundle takes.
func bundleBytes(t *testing.T, home string) (n int64) {
	t.Helper()
	check(filepath.WalkDir(filepath.Join(home, storeBundle), func(p string, d fs.DirEntry, e error) error {
		check(e)
		info, e := d.Info()
		check(e)
		n += info.Sys().(*syscall.Stat_t).Blocks * 512
		return nil
	}))
	return n
}

// loadCasePair docker-loads test/case:1, one layer with linux/netfilter's pair
// (golang:1.26 has 11 such pairs).
func loadCasePair(t *testing.T, b *engine) {
	t.Helper()
	pair := layer(t, []tar.Header{{Name: "netfilter/", Typeflag: tar.TypeDir, Mode: 0o755}, {Name: "netfilter/xt_CONNMARK.h", Size: 8, Mode: 0o644}, {Name: "netfilter/xt_connmark.h", Size: 8, Mode: 0o644}},
		[]string{"", "CONNMARK", "connmark"})
	config, _ := fixtureBlobs(t)
	c := decode(config, maxJSON)
	object(c["rootfs"])["diff_ids"] = []any{digest(pair)}
	raw, e := json.Marshal(c)
	check(e)
	if _, failure := load(t, b, classicArchive(t, raw, pair, `["test/case:1"]`), ""); failure != "" {
		t.Fatal(failure)
	}
}
func checkCasePair(t *testing.T, root string) {
	t.Helper()
	for name, want := range map[string]string{"xt_CONNMARK.h": "CONNMARK", "xt_connmark.h": "connmark"} {
		if data, e := os.ReadFile(filepath.Join(root, "netfilter", name)); string(data) != want {
			t.Fatalf("%s: %q %v", name, data, e)
		}
	}
}

func TestVolumeNamesByCase(t *testing.T) {
	b := testEngine(t)
	b.createVolume("Data", obj{}, false)
	e := safeRun(func() { b.createVolume("data", obj{}, false) })
	if e == nil { // a case-sensitive home: two volumes
		if b.volumeInfo("data")["Mountpoint"] == b.volumeInfo("Data")["Mountpoint"] {
			t.Fatal("data and Data share a directory")
		}
		return
	}
	var refused apiError
	if !errors.As(e, &refused) || refused.Code != 409 {
		t.Fatalf("data beside Data: %v", e)
	}
	mustReject(t, func() { b.volumeMeta("data") })
	mustReject(t, func() { b.removeVolume("data") })
	b.volumeMeta("Data")
}

// An engine home from before the store moves into it at a start with no container
// running; an interrupted move goes on.
func TestMoveToStore(t *testing.T) {
	home := t.TempDir()
	check(os.Chmod(home, 0o700))
	old := engineAt(home, home)
	h := old.socketHandler()
	for _, name := range []string{"web", "live"} {
		if r := request(t, h, "POST", "/containers/create?name="+name, `{"Image":"dockiva/shell:demo","Cmd":["-c","true"]}`, ""); r.Code != 201 {
			t.Fatalf("create %s: %d %s", name, r.Code, r.Body.String())
		}
	}
	// The container's own files: what a copy across volumes must keep.
	upper := filepath.Join(home, "containers", "web", "rootfs")
	check(os.MkdirAll(filepath.Join(upper, "ro"), 0o700))
	check(os.WriteFile(filepath.Join(upper, "ro", "f"), []byte("ro"), 0o644))
	check(os.WriteFile(filepath.Join(upper, "h1"), []byte("hard"), 0o644))
	check(os.Link(filepath.Join(upper, "h1"), filepath.Join(upper, "h2")))
	check(os.WriteFile(filepath.Join(upper, "suid"), []byte("s"), 0o644))
	check(setMode(filepath.Join(upper, "suid"), 0o4755|os.ModeSetuid))
	check(os.WriteFile(filepath.Join(upper, "secret"), []byte("x"), 0o600))
	check(os.Chmod(filepath.Join(upper, "secret"), 0))
	check(os.Symlink("/ro/f", filepath.Join(upper, "ln")))
	check(syscall.Mkfifo(filepath.Join(upper, "fifo"), 0o644))
	check(os.Mkdir(filepath.Join(upper, "tmp"), 0o700))
	check(setMode(filepath.Join(upper, "tmp"), 0o777|os.ModeSticky))
	short, e := os.MkdirTemp("/tmp", "s") // a socket path has 104 bytes at most
	check(e)
	defer os.RemoveAll(short)
	l, e := net.Listen("unix", filepath.Join(short, "s"))
	check(e)
	l.(*net.UnixListener).SetUnlinkOnClose(false)
	l.Close()
	check(os.Rename(filepath.Join(short, "s"), filepath.Join(upper, "sock")))
	check(os.Chmod(filepath.Join(upper, "ro"), 0o555))
	check(os.MkdirAll(filepath.Join(home, "lowers", "abc"), 0o700))
	check(os.WriteFile(filepath.Join(home, "lowers", "abc", "f"), []byte("lower"), 0o644))
	check(os.MkdirAll(filepath.Join(home, "build-cache", "k"), 0o700))
	check(os.WriteFile(filepath.Join(home, "build-cache", "k", "entry.json"), []byte("{}"), 0o600))
	check(os.Mkdir(filepath.Join(home, "containers", ".creating-x"), 0o700))
	check(os.WriteFile(filepath.Join(home, "lowers", "abc.pack"), []byte("stale"), 0o600))
	image := filepath.Dir(old.image("dockiva/shell:demo").Root)
	key := filepath.Base(image)
	// Packed small files (an index by inode: useless copied), a docker tag, and an image
	// unpacked on the Mac's disk (one file of the pair, where it is case-insensitive).
	check(os.WriteFile(filepath.Join(image, "rootfs.pack"), []byte("stale"), 0o600))
	commitImage(old.Cache, "dockiva/shell:tag", filepath.Join(image, "layout"))
	loadCasePair(t, old)

	// A running container keeps the home as it is.
	lock, e := lockFile(filepath.Join(home, "containers", "live", "lock"), true)
	check(e)
	if openStore(home) != home || exists(filepath.Join(home, storeBundle)) {
		t.Fatal("store made while a container runs")
	}
	lock.Close()
	store := openStore(home)
	t.Cleanup(func() { _ = exec.Command("/usr/bin/hdiutil", "detach", "-force", store).Run() })

	// An interrupted move of lowers/abc: renamed aside, a partial copy in the store.
	check(os.Rename(filepath.Join(home, "lowers", "abc"), filepath.Join(home, "lowers", ".moving-abc")))
	check(os.MkdirAll(filepath.Join(store, "lowers", ".moving-abc", "partial"), 0o700))
	moveToStore(home, store)

	for _, dir := range []string{"images", "lowers", "build-cache", "containers"} {
		if exists(filepath.Join(home, dir)) {
			t.Fatalf("%s left in the home", dir)
		}
	}
	b := engineAt(home, store)
	if names := b.names(); !slices.Contains(names, "web") || !slices.Contains(names, "live") || exists(filepath.Join(store, "containers", ".creating-x")) {
		t.Fatalf("containers: %v", names)
	}
	lower := text(readObject(filepath.Join(store, "containers", "web", "config.json"))["lower"])
	if lower != filepath.Join(store, "images", key, "rootfs") || !exists(filepath.Join(lower, "bin")) {
		t.Fatalf("lower: %s", lower)
	}
	// Images are unpacked again in the store: whole pairs; a tag shares its image's files.
	checkCasePair(t, b.image("test/case:1").Root)
	if tag := b.image("dockiva/shell:tag"); tag.Digest != b.image("dockiva/shell:demo").Digest || !exists(filepath.Join(tag.Root, "bin")) ||
		exists(filepath.Join(store, "images", key, "rootfs.pack")) || exists(filepath.Join(store, "lowers", "abc.pack")) {
		t.Fatal("tag or packs")
	}
	upper = filepath.Join(store, "containers", "web", "rootfs")
	stat := func(name string) fs.FileInfo {
		info, e := os.Lstat(filepath.Join(upper, name))
		check(e)
		return info
	}
	if !os.SameFile(stat("h1"), stat("h2")) || stat("suid").Mode()&permBits != 0o755|os.ModeSetuid || stat("secret").Mode().Perm() != 0 ||
		stat("ro").Mode().Perm() != 0o555 || stat("tmp").Mode()&permBits != 0o777|os.ModeSticky || stat("fifo").Mode()&fs.ModeNamedPipe == 0 {
		t.Fatalf("modes or links not kept: suid %v ro %v tmp %v secret %v", stat("suid").Mode(), stat("ro").Mode(), stat("tmp").Mode(), stat("secret").Mode())
	}
	if target, _ := os.Readlink(filepath.Join(upper, "ln")); target != "/ro/f" || exists(filepath.Join(upper, "sock")) {
		t.Fatalf("symlink %q, socket copied %v", target, exists(filepath.Join(upper, "sock")))
	}
	if data, _ := os.ReadFile(filepath.Join(store, "lowers", "abc", "f")); string(data) != "lower" || exists(filepath.Join(store, "lowers", ".moving-abc")) {
		t.Fatalf("interrupted move: %q", data)
	}
	if !exists(filepath.Join(store, "build-cache", "k", "entry.json")) {
		t.Fatal("build cache not moved")
	}
	check(os.Chmod(filepath.Join(upper, "ro"), 0o755)) // for the temporary directory's removal
}

// Under minFree the guard pauses the running containers and refuses what writes
// much, saying why; with room again it unpauses the containers it paused.
func TestLowSpaceGuard(t *testing.T) {
	b := testEngine(t)
	h := b.handler("")
	b.create("web", decode([]byte(`{"Image":"dockiva/shell:demo"}`), maxJSON))
	p := b.container("web")
	run := exec.Command("/bin/sleep", "30") // stands in for the worker, its own group leader
	run.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	check(run.Start())
	defer func() { _ = run.Process.Kill(); _ = run.Wait() }()
	lock, e := lockFile(filepath.Join(p, "lock"), true) // a live supervisor holds it
	check(e)
	defer lock.Close()
	save(filepath.Join(p, "status.json"), obj{"state": "running", "generation": "g", "pid": run.Process.Pid})
	paused := func() bool { return object(b.inspect("web")["State"])["Paused"] == true }
	defer func(was uint64) { minFree = was }(minFree)
	guard := map[string]bool{}

	minFree = 1 << 62 // more than the disk has
	b.checkSpace(guard)
	if !paused() {
		t.Fatal("running container not paused")
	}
	for _, path := range []string{"/containers/create?name=next", "/containers/web/unpause", "/images/create?fromImage=alpine", "/images/load", "/build", "/commit?container=web"} {
		if r := request(t, h, "POST", path, "{}", ""); r.Code != 507 || !strings.Contains(r.Body.String(), "almost full") {
			t.Fatalf("%s with the Mac almost full: %d %s", path, r.Code, r.Body.String())
		}
	}
	if r := request(t, h, "GET", "/containers/json?all=1", "", ""); r.Code != 200 {
		t.Fatalf("list with the Mac almost full: %d", r.Code)
	}

	minFree = 0
	b.checkSpace(guard)
	if paused() || b.lowSpace.Load() != nil {
		t.Fatal("still paused with room again")
	}
	if r := request(t, h, "POST", "/containers/create?name=next", `{"Image":"dockiva/shell:demo"}`, ""); r.Code != 201 {
		t.Fatalf("create with room again: %d %s", r.Code, r.Body.String())
	}
}
