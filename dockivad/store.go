package main

// The store: images, containers, retained lowers, the build cache and builds' work
// live on a case-sensitive APFS volume, a sparse bundle in the engine home
// (store.sparsebundle) attached at <home>/store. On the Mac's case-insensitive APFS
// two Linux names that differ only by case are one file: dpkg failed on packages
// holding such pairs (libffi-dev's index.html and Index.html, through its
// NAME.dpkg-new files), image unpack kept one file of each pair under both names
// (xt_CONNMARK.h and xt_connmark.h), write-back of the memory layer dropped one,
// and containers Web and web shared a directory. Volumes, networks, sockets and
// process links stay in <home>: a volume's files keep the Mac's names, as bind mounts
// do on every Mac engine, along with Time Machine and the boot volume's flushes.
//
// The bundle is attached with diskutil image attach, never hdiutil attach, which
// turns every F_BARRIERFSYNC into a full flush (10.8 ms instead of 0.9). It holds
// the APFS container alone, with no partition map: hdiutil's default GPT layout adds
// an EFI partition, which DiskArbitration probes through FSKit (msdos) at every
// attach, and attaches waiting on those probes took from 11 s to over 10 minutes.
// Stores made with that layout attach as they are. The engine answers /_ping during
// a slow attach (main.go). It grows as
// files are written and keeps its size when they go: closeStore gives the space back
// when the engine stops (POST /dockiva/shutdown). guardSpace keeps it from filling
// the Mac's disk, which would eject it.

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

const storeBundle = "store.sparsebundle"

// storeDir is where the engine keeps images, containers, retained lowers and the
// build cache: <home>/store, or <home> itself for an engine started before its
// containers moved (openStore).
func (b *engine) storeDir() string { return filepath.Dir(b.State) }

// openStore attaches the store, made first on a new home, and returns its root,
// <home>/store, checked case-sensitive. While containers an engine before the store
// left running use <home>'s files, it returns <home>: they move at a start with none
// running (moveToStore).
func openStore(home string) string {
	bundle, root := filepath.Join(home, storeBundle), filepath.Join(home, "store")
	if !exists(bundle) {
		if names := runningIn(filepath.Join(home, "containers")); len(names) > 0 {
			fmt.Fprintln(os.Stderr, "dockivad: containers move to the case-sensitive store at a start with none running; running:", strings.Join(names, ", "))
			return home
		}
		stage := filepath.Join(home, ".creating-"+storeBundle) // hdiutil adds .sparsebundle to a name without it
		check(os.RemoveAll(stage))
		command("/usr/bin/hdiutil", "create", "-quiet", "-type", "SPARSEBUNDLE", "-fs", "Case-sensitive APFS", "-layout", "NONE", "-volname", "Dockiva Engine", "-size", "4t", stage)
		check(os.Rename(stage, bundle))
		// Time Machine would copy the changed bands: images and containers, which can be
		// pulled and made again (volumes stay in <home>, backed up). The attribute tmutil
		// addexclusion sets (a plist of "com.apple.backupd"); tmutil itself took 11 s.
		command("/usr/bin/xattr", "-wx", "com.apple.metadata:com_apple_backup_excludeItem", "62706C69737430305F1011636F6D2E6170706C652E6261636B75706408000000000000010100000000000000010000000000000000000000000000001C", bundle)
	}
	check(os.MkdirAll(root, 0o700))
	if !mounted(root) {
		command("/usr/sbin/diskutil", "image", "attach", "--nobrowse", "--mountPoint", root, bundle)
		if !mounted(root) { // diskutil answers with the mount point of an image attached already
			panic(fmt.Errorf("%s is attached elsewhere; detach it (hdiutil detach) and start the engine again", bundle))
		}
	}
	check(os.Chmod(root, 0o700)) // a new volume's root is 0755
	probe := filepath.Join(root, ".Case")
	check(os.WriteFile(probe, nil, 0o600))
	_, e := os.Lstat(filepath.Join(root, ".case"))
	check(os.Remove(probe))
	if !errors.Is(e, fs.ErrNotExist) {
		panic(fmt.Errorf("%s is not case-sensitive", root))
	}
	return private(root)
}

// closeStore detaches the store once the containers stopped (POST /dockiva/shutdown;
// a request may read a container's files a moment longer), then gives its free space
// back to the Mac: hdiutil compact of the detached bundle. APFS hands freed blocks
// back to the bundle only after some transactions: 128 barriers first (50 did it,
// 30 did not; nor did 45 s of waiting, or syncs). ponytail: only at a stop, not after
// a restart of the Mac (compacting at start would delay the API).
func closeStore(home string) {
	root := filepath.Join(home, "store")
	if !mounted(root) {
		return
	}
	if f, e := os.CreateTemp(root, ".settle-"); e == nil {
		block := make([]byte, 16384)
		for i := 0; i < 128; i++ {
			if _, e := f.WriteAt(block, int64(i)*int64(len(block))); e != nil || barrierSync(f) != nil {
				break
			}
		}
		f.Close()
		os.Remove(f.Name())
	}
	if !detachStore(home, 5) {
		return
	}
	if out, e := exec.Command("/usr/bin/hdiutil", "compact", "-quiet", filepath.Join(home, storeBundle)).CombinedOutput(); e != nil {
		fmt.Fprintln(os.Stderr, "dockivad: hdiutil compact:", strings.TrimSpace(string(out)))
	}
}

// detachStore detaches the store unless something still uses it (a running
// container's supervisor), trying tries times 200 ms apart: at any exit of the engine
// (SIGTERM leaves containers running, and keeps it attached for them), so the home
// can go once the engine has.
func detachStore(home string, tries int) bool {
	root := filepath.Join(home, "store")
	if !mounted(root) {
		return false
	}
	var out []byte
	var e error
	for try := 0; try < tries; try++ {
		if try > 0 {
			time.Sleep(200 * time.Millisecond)
		}
		if out, e = exec.Command("/usr/bin/hdiutil", "detach", "-quiet", root).CombinedOutput(); e == nil {
			return true
		}
	}
	fmt.Fprintln(os.Stderr, "dockivad: the store stays attached:", strings.TrimSpace(string(out)))
	return false
}

// minFree is the Mac's free space under which guardSpace pauses the containers.
var minFree uint64 = 4 << 30

// guardSpace keeps the Mac's disk from filling under the store. The bundle grows by
// every block the containers write (APFS in it reuses no freed block while attached:
// 7 x 300 MB written and deleted grew it by 2.1 GB), and when the Mac's volume runs
// out, macOS ejects it mid-write (DiskImages2: "unrecoverable I/O error"): the writer
// gets EIO, the volume is force-unmounted, the writes since its last checkpoint are
// lost, every running container loses its files and its supervisor its lock, and the
// engine goes on without a store. APFS answers ENOSPC only once the Mac is full
// already: writers of 50 and 380 MB/s got there first. So under minFree the running
// containers are paused and starts, pulls, loads, builds, commits and copies in are
// refused (writesMuch, 507) until the Mac has a GiB more; then the containers the
// guard paused go on. The store's statfs reports the Mac's free space (its size is
// the bundle's 4 TB). ponytail: pulls and builds under way go on; an ENOSPC from the
// runtime (writes checked against the Mac's free space) would stop those too, and
// give writers Docker's error instead of a pause.
func (b *engine) guardSpace(stop <-chan struct{}) {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	paused := map[string]bool{}
	for {
		b.checkSpace(paused)
		select {
		case <-stop:
			return
		case <-tick.C:
		}
	}
}

// checkSpace is one pass of guardSpace; paused holds the containers it paused.
func (b *engine) checkSpace(paused map[string]bool) {
	var s syscall.Statfs_t
	if syscall.Statfs(b.storeDir(), &s) != nil {
		return
	}
	free, low := s.Bavail*uint64(s.Bsize), b.lowSpace.Load() != nil
	switch {
	case low && free >= minFree+1<<30:
		b.lowSpace.Store(nil)
		fmt.Fprintln(os.Stderr, "dockivad: the Mac has free space again; the containers paused for it go on")
		for name := range paused {
			_ = safeRun(func() { b.pause(name, false) })
		}
		clear(paused)
		return
	case !low && free >= minFree:
		return
	}
	why := fmt.Sprintf("The Mac's disk is almost full (%d MB free; Dockiva Engine keeps %d MB): its containers are paused, and starts, pulls and builds refused, until there is more. Images and containers removed in the engine give their space back when it stops", free>>20, minFree>>20)
	if !low {
		fmt.Fprintln(os.Stderr, "dockivad:", why)
	}
	b.lowSpace.Store(&why)
	var names []string
	_ = safeRun(func() { names = b.names() })
	for _, name := range names { // every pass: containers started before the refusals too
		if !paused[name] && safeRun(func() { b.pause(name, true) }) == nil {
			paused[name] = true
		}
	}
}

// writesMuch: the requests refused while the Mac is almost full (guardSpace).
func writesMuch(method, path string) bool {
	if method == "POST" {
		switch path {
		case "/containers/create", "/images/create", "/images/load", "/build", "/grpc", "/session", "/commit":
			return true
		}
	}
	m := containerPath.FindStringSubmatch(path)
	return m != nil && (method == "POST" && (m[2] == "start" || m[2] == "restart" || m[2] == "unpause") || method == "PUT" && m[2] == "archive")
}

// mounted reports whether a volume is mounted at dir.
func mounted(dir string) bool {
	var d, parent syscall.Stat_t
	return syscall.Lstat(dir, &d) == nil && syscall.Lstat(filepath.Dir(dir), &parent) == nil && d.Dev != parent.Dev
}

// engineHome is the engine home of container directory p, where its networks,
// volumes, ports and process links are: p is <home>/store/containers/<name>, or
// <home>/containers/<name> before the store.
func engineHome(p string) string {
	root := filepath.Dir(filepath.Dir(p))
	if filepath.Base(root) == "store" && exists(filepath.Join(filepath.Dir(root), storeBundle)) {
		return filepath.Dir(root)
	}
	return root
}

// command runs a macOS tool; its output is the error when it fails.
func command(name string, args ...string) {
	if out, e := exec.Command(name, args...).CombinedOutput(); e != nil {
		panic(fmt.Errorf("%s %s: %v: %s", filepath.Base(name), args[0], e, strings.TrimSpace(string(out))))
	}
}

// runningIn lists the containers in state whose supervisor holds their lock.
func runningIn(state string) []string {
	entries, _ := os.ReadDir(state)
	out := []string{}
	for _, entry := range entries {
		if running(filepath.Join(state, entry.Name())) {
			out = append(out, entry.Name())
		}
	}
	return out
}
func running(p string) bool {
	fd, e := syscall.Open(filepath.Join(p, "lock"), syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
	if e != nil {
		return false
	}
	defer syscall.Close(fd)
	return syscall.Flock(fd, syscall.LOCK_EX|syscall.LOCK_NB) != nil
}

// moveToStore moves what an engine before the store kept in <home> (images, retained
// lowers, the build cache and containers) into the store, entry by entry: renamed
// aside (.moving-NAME), copied into the store under that name (another volume: no
// clones; images unpacked again, moveImage), renamed into place there, then removed
// from <home>; an interrupted move goes on at the next start. Moved containers'
// configs follow their lowers. An entry the store has already, or a container still
// running, stays where it is; the engines' temporary entries (other dot names) and
// the lowers' packed small files (their index is by inode: the first run makes them
// again) go.
func moveToStore(home, store string) {
	moved, start := 0, time.Now()
	unpacked := map[string]string{} // image digest: its moved directory, whose files its tags clone
	for _, dir := range []string{"images", "lowers", "build-cache", "containers"} {
		from, to := filepath.Join(home, dir), filepath.Join(store, dir)
		entries, e := os.ReadDir(from)
		if errors.Is(e, fs.ErrNotExist) {
			continue
		}
		check(e)
		check(os.MkdirAll(to, 0o700))
		for _, entry := range entries {
			name, src := entry.Name(), filepath.Join(from, entry.Name())
			if strings.HasPrefix(name, ".") && !strings.HasPrefix(name, ".moving-") || dir == "lowers" && strings.Contains(name, ".pack") {
				check(removeTree(src))
				continue
			}
			if !strings.HasPrefix(name, ".moving-") {
				if exists(filepath.Join(to, name)) || dir == "containers" && running(src) {
					fmt.Fprintln(os.Stderr, "dockivad: not moved to the store (in it already, or running):", src)
					continue
				}
				aside := filepath.Join(from, ".moving-"+name)
				check(os.Rename(src, aside))
				src = aside
			}
			name = strings.TrimPrefix(name, ".moving-")
			if dst, stage := filepath.Join(to, name), filepath.Join(to, ".moving-"+name); !exists(dst) {
				check(removeTree(stage))
				switch dir {
				case "images":
					moveImage(src, stage, dst, unpacked)
				case "containers":
					copyAcross(src, stage)
					followLower(stage, store)
				default:
					copyAcross(src, stage)
				}
				check(os.Rename(stage, dst))
				moved++
			}
			check(removeTree(src))
		}
		_ = os.Remove(from) // unless something stayed
	}
	for _, dir := range []string{"build", "load"} { // builds' and loads' work, in the store now
		_ = os.Remove(filepath.Join(home, dir))
	}
	if moved > 0 {
		fmt.Fprintf(os.Stderr, "dockivad: moved %d images, containers and cache entries to the case-sensitive store in %s\n", moved, time.Since(start).Round(100*time.Millisecond))
	}
}

// moveImage copies a cached image to stage (on its way to dst) without its unpacked
// files, then unpacks it again from its layers: unpacked on the case-insensitive disk,
// one file of each pair of names that differ only by case was lost (golang:1.26's
// xt_CONNMARK.h held xt_connmark.h's content), and a pull of the same digest would
// keep them. A tag of an image moved already clones its layout and files, as docker
// tag does. The packed small files (rootfs.pack: an index by inode) go; the first run
// makes them again. An image that does not unpack keeps a copy of its old files.
func moveImage(src, stage, dst string, unpacked map[string]string) {
	digest := ""
	_ = safeRun(func() { digest = text(readObject(filepath.Join(src, "image.json"))["digest"]) })
	same := unpacked[digest]
	entries, e := os.ReadDir(src)
	check(e)
	check(os.Mkdir(stage, 0o700))
	for _, entry := range entries {
		switch n := entry.Name(); {
		case n == "rootfs" || strings.HasPrefix(n, "rootfs.pack") || strings.HasPrefix(n, "."):
		case n == "layout" && same != "":
			cloneTree(filepath.Join(same, n), filepath.Join(stage, n))
		default:
			copyAcross(filepath.Join(src, n), filepath.Join(stage, n))
		}
	}
	if same != "" {
		cloneTree(filepath.Join(same, "rootfs"), filepath.Join(stage, "rootfs"))
		return
	}
	if e := safeRun(func() { ensureUnpacked(stage) }); e != nil && exists(filepath.Join(src, "rootfs")) {
		fmt.Fprintln(os.Stderr, "dockivad: "+src+" did not unpack again, its files are copied:", e)
		copyAcross(filepath.Join(src, "rootfs"), filepath.Join(stage, "rootfs"))
	}
	if digest != "" && exists(filepath.Join(stage, "rootfs")) {
		unpacked[digest] = dst
	}
}

// followLower points a moved container's config at its lower layer's place in the
// store: <old home>/images/<key>/rootfs or <old home>/lowers/<id>.
func followLower(p, store string) {
	config := filepath.Join(p, "config.json")
	if !exists(config) {
		return
	}
	c := readObject(config)
	l := text(c["lower"])
	switch {
	case l == "":
		return
	case filepath.Base(l) == "rootfs" && filepath.Base(filepath.Dir(filepath.Dir(l))) == "images":
		c["lower"] = filepath.Join(store, "images", filepath.Base(filepath.Dir(l)), "rootfs")
	case filepath.Base(filepath.Dir(l)) == "lowers":
		c["lower"] = filepath.Join(store, "lowers", filepath.Base(l))
	default:
		return
	}
	save(config, c)
}

// copyAcross copies a tree to another volume, where clonefile cannot reach: files,
// directories, symlinks, FIFOs and hard links, each with its mode (setuid, setgid
// and sticky too, which ditto drops) and modification time. Sockets and devices are
// left out: a socket's process is gone, and no layer holds devices.
// ponytail: links are kept within the tree only, so a tagged image's copies share
// nothing (they were APFS clones); dedupe by digest if tags are common.
func copyAcross(src, dst string) {
	type dir struct {
		path string
		mode fs.FileMode
		time time.Time
	}
	dirs, links := []dir{}, map[uint64]string{}
	check(filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		check(err)
		rel, e := filepath.Rel(src, p)
		check(e)
		to := filepath.Join(dst, rel)
		info, e := d.Info()
		check(e)
		st := info.Sys().(*syscall.Stat_t)
		need := fs.FileMode(0o400) // a guest's chmod 000: made readable to copy (the source goes after)
		if d.IsDir() {
			need = 0o500
		}
		if d.Type()&fs.ModeSymlink == 0 && info.Mode().Perm()&need != need {
			check(os.Chmod(p, info.Mode().Perm()|need))
		}
		switch t := d.Type(); {
		case t.IsDir():
			check(os.Mkdir(to, 0o700))
			dirs = append(dirs, dir{to, info.Mode() & permBits, info.ModTime()})
			return nil
		case t&fs.ModeSymlink != 0:
			target, e := os.Readlink(p)
			check(e)
			return os.Symlink(target, to)
		case t&fs.ModeNamedPipe != 0:
			check(syscall.Mkfifo(to, 0o600))
		case !t.IsRegular():
			return nil
		case links[st.Ino] != "":
			return os.Link(links[st.Ino], to)
		default:
			linkOrCopy(p, to) // the link fails across volumes: a copy
			if st.Nlink > 1 {
				links[st.Ino] = to
			}
		}
		check(setMode(to, info.Mode()&permBits))
		return os.Chtimes(to, info.ModTime(), info.ModTime())
	}))
	for i := len(dirs) - 1; i >= 0; i-- { // children first: a read-only directory is set once filled
		check(setMode(dirs[i].path, dirs[i].mode))
		check(os.Chtimes(dirs[i].path, dirs[i].time, dirs[i].time))
	}
}
