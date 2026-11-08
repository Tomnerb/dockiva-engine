package main

// The build cache, as Docker's classic builder keeps one: each RUN, COPY and ADD
// result is the stage's files after the step, kept as an APFS clone under
// <store>/build-cache/<key>/ (rootfs, the stage config, and entry.json: the
// /system/df BuildCache row). The key is the stage's state before the step (the
// base image digest, chained through every step), the instruction, the ARGs and
// settings it sees, for COPY and ADD a hash of the sources, and for RUN a hash of
// its bind mounts' sources (cache mounts and secret values are not part of it).
// ponytail: ADD of a URL is never cached and leaves the rest of its stage uncached
// (Docker downloads it to hash it); download first and key on the bytes if that hurts.

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"syscall"
	"time"
)

// buildCacheMu orders lookups, stores and evictions across concurrent builds.
var buildCacheMu sync.Mutex

// buildCacheMax caps the build cache, together with a tenth of the free disk.
var buildCacheMax int64 = 10 << 30

func (b *engine) buildCacheDir() string { return private(filepath.Join(b.storeDir(), "build-cache")) }

// stepKey is a step's cache key: the stage's state, the instruction (with its
// here-documents), and what it sees.
func stepKey(st *buildStage, in instruction, extra string) string {
	parts := []any{st.cacheID, in.original, st.args, st.config, extra}
	for _, h := range in.heredocs {
		parts = append(parts, h.body)
	}
	raw, e := json.Marshal(parts)
	check(e)
	return digest(raw)[7:]
}

// chain moves the stage's state past a settings-only step (ENV, WORKDIR, ARG, ...).
func (bd *builder) chain(st *buildStage, in instruction) {
	if st.cacheID != "" {
		st.cacheID = stepKey(st, in, "")
	}
}

// cached runs a RUN, COPY or ADD step unless the build cache holds its result.
// extra is what else the step reads (COPY's sources); ok false leaves this step
// and the rest of the stage uncached.
func (bd *builder) cached(st *buildStage, in instruction, extra string, ok bool, run func()) {
	if !ok || st.cacheID == "" {
		run()
		st.cacheID = ""
		return
	}
	id := stepKey(st, in, extra)
	dir := filepath.Join(bd.b.buildCacheDir(), id)
	if !bd.nocache && bd.restore(st, dir) {
		bd.say(" ---> Using cache\n")
	} else {
		run()
		bd.store(st, in, id, dir)
	}
	st.cacheID = id
	bd.say(" ---> " + id[:12] + "\n")
}

// restore makes a cached result the stage's files, reporting whether there was one.
func (bd *builder) restore(st *buildStage, dir string) bool {
	buildCacheMu.Lock()
	defer buildCacheMu.Unlock()
	var entry, config obj
	if safeRun(func() {
		entry, config = readObject(filepath.Join(dir, "entry.json")), readObject(filepath.Join(dir, "config.json"))
	}) != nil {
		return false
	}
	bd.discardRoot(st)
	cloneTree(filepath.Join(dir, "rootfs"), st.root)
	st.config = config
	entry["LastUsedAt"], entry["UsageCount"] = time.Now().UTC().Format(time.RFC3339Nano), integer(entry["UsageCount"])+1
	save(filepath.Join(dir, "entry.json"), entry)
	return true
}

// store caches the stage's files after a step, then evicts the least recently
// used entries over the cap. The cache is best effort: a failure here is not the build's.
func (bd *builder) store(st *buildStage, in instruction, id, dir string) {
	_ = safeRun(func() {
		tmp, e := os.MkdirTemp(bd.work, "cache-")
		check(e)
		defer os.RemoveAll(tmp)
		cloneTree(st.root, filepath.Join(tmp, "rootfs"))
		now := time.Now().UTC().Format(time.RFC3339Nano)
		save(filepath.Join(tmp, "config.json"), st.config)
		save(filepath.Join(tmp, "entry.json"), obj{"ID": id, "Type": "regular", "Description": in.original, "InUse": false, "Shared": false,
			"Size": changedSize(filepath.Join(tmp, "rootfs"), st.baseRoot), "CreatedAt": now, "LastUsedAt": now, "UsageCount": 0})
		buildCacheMu.Lock()
		defer buildCacheMu.Unlock()
		bd.b.discard(dir) // a --no-cache rebuild replaces the entry
		check(os.Rename(tmp, dir))
		bd.b.pruneBuildCache(buildCacheLimit(filepath.Dir(dir)), func(time.Time) bool { return true })
	})
}

// basePull is a build's image of arch where the tag holds another architecture (or,
// for amd64, nothing): pulled once and kept as a build cache entry (its layout and
// files beside entry.json), which docker builder prune removes. The build holds the
// entry until it ends (buildCacheInUse): its commit reads the blobs and files.
func (bd *builder) basePull(ref, arch string) *image {
	id := key("image\x00" + arch + "\x00" + shortName(ref))
	dir := filepath.Join(bd.b.buildCacheDir(), id)
	use := func() bool { // the caller holds buildCacheMu
		if safeRun(func() {
			entry := readObject(filepath.Join(dir, "entry.json"))
			entry["LastUsedAt"], entry["UsageCount"] = time.Now().UTC().Format(time.RFC3339Nano), integer(entry["UsageCount"])+1
			save(filepath.Join(dir, "entry.json"), entry)
		}) != nil {
			return false
		}
		buildCacheInUse[id]++
		bd.held = append(bd.held, id)
		return true
	}
	buildCacheMu.Lock()
	ok := use()
	buildCacheMu.Unlock()
	if !ok {
		bd.say("Pulling " + ref + " (linux/" + arch + ")\n")
		pullFor(ref, arch, bd.work, bd.b.Insecure, credentials{}, func(name, layout string) string {
			stage, e := os.MkdirTemp(bd.work, "image-")
			check(e)
			entry := filepath.Dir(commitImage(stage, name, layout))
			now := time.Now().UTC().Format(time.RFC3339Nano)
			save(filepath.Join(entry, "entry.json"), obj{"ID": id, "Type": "regular", "Description": "pulled from " + name + " (linux/" + arch + ")", "InUse": false, "Shared": false,
				"Size": integer(readObject(filepath.Join(entry, "image.json"))["size"]), "CreatedAt": now, "LastUsedAt": now, "UsageCount": 0})
			buildCacheMu.Lock()
			defer buildCacheMu.Unlock()
			if !exists(filepath.Join(dir, "entry.json")) { // else a concurrent build pulled it first
				bd.b.discard(dir)
				check(os.Rename(entry, dir))
			}
			reject(use(), "The build cache lost "+ref+" (linux/"+arch+")")
			return filepath.Join(dir, "layout")
		})
	}
	im := loadImage(noLinks(dir, "layout"))
	im.Root = noLinks(dir, "rootfs")
	return im
}

// release lets go of the build cache entries the build held (basePull).
func (bd *builder) release() {
	buildCacheMu.Lock()
	defer buildCacheMu.Unlock()
	for _, id := range bd.held {
		if buildCacheInUse[id]--; buildCacheInUse[id] <= 0 {
			delete(buildCacheInUse, id)
		}
	}
	bd.held = nil
}

// changedSize is the bytes of root's files that differ from base's (by size and
// mtime): what a cached root holds beyond the image it is cloned from.
// ponytail: an estimate; entries of one build share the blocks of the files they all
// changed, so the sum overstates the disk used. Ask APFS for private sizes if it matters.
func changedSize(root, base string) int64 {
	var n int64
	check(filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		check(err)
		if !d.Type().IsRegular() {
			return nil
		}
		info, e := d.Info()
		check(e)
		if base != "" {
			rel, e := filepath.Rel(root, p)
			check(e)
			if old, e := os.Lstat(filepath.Join(base, rel)); e == nil && old.Mode().IsRegular() && old.Size() == info.Size() && old.ModTime().Equal(info.ModTime()) {
				return nil
			}
		}
		n += info.Size()
		return nil
	}))
	return n
}

// buildCacheLimit is the cache's cap: buildCacheMax, or a tenth of the free disk if less.
func buildCacheLimit(dir string) int64 {
	var s syscall.Statfs_t
	if syscall.Statfs(dir, &s) != nil {
		return buildCacheMax
	}
	return min(buildCacheMax, int64(s.Bavail)*int64(s.Bsize)/10)
}

// sourceHash is what a COPY or ADD reads: each source's names, modes, contents and
// symlink targets. ok is false for an ADD of a URL and for a missing source (the
// step then fails as it runs).
func (bd *builder) sourceHash(st *buildStage, in instruction, env map[string]string) (string, bool) {
	if st.cacheID == "" {
		return "", false
	}
	args, root := bd.copyArgs(st, in, env)
	h := sha256.New()
	for _, src := range args[:len(args)-1] {
		if in.heredocFor(src) != nil {
			continue // in the key with the instruction
		}
		matches := []globMatch{}
		if in.cmd != "ADD" || !isURL(src) {
			matches = globWithin(root, src)
		}
		if len(matches) == 0 {
			return "", false
		}
		for _, m := range matches {
			fmt.Fprintf(h, "%q\n", m.name)
			hashTree(h, m.host)
		}
	}
	return hex.EncodeToString(h.Sum(nil)), true
}

// treeHash is a hash of a file or directory tree (hashTree).
func treeHash(root string) string {
	h := sha256.New()
	hashTree(h, root)
	return hex.EncodeToString(h.Sum(nil))
}

// hashTree writes each entry's name, mode, size and content or symlink target.
func hashTree(h io.Writer, root string) {
	check(filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		check(err)
		rel, e := filepath.Rel(root, p)
		check(e)
		info, e := d.Info()
		check(e)
		fmt.Fprintf(h, "%q %x %d ", rel, uint32(info.Mode()), info.Size())
		switch {
		case info.Mode()&fs.ModeSymlink != 0:
			target, e := os.Readlink(p)
			check(e)
			fmt.Fprintf(h, "%q", target)
		case info.Mode().IsRegular():
			f, e := os.OpenFile(p, os.O_RDONLY|syscall.O_NOFOLLOW, 0)
			check(e)
			defer f.Close()
			_, e = io.CopyN(h, f, info.Size())
			check(e)
		}
		fmt.Fprintln(h)
		return nil
	}))
}

// buildCacheEntries are the cache's entry.json rows, as /system/df lists them.
func (b *engine) buildCacheEntries() []any {
	dir := b.buildCacheDir()
	names, e := os.ReadDir(dir)
	check(e)
	out := []any{}
	for _, n := range names {
		_ = safeRun(func() {
			entry := readObject(filepath.Join(dir, n.Name(), "entry.json"))
			reject(entry["ID"] == n.Name(), "Mismatched build cache entry")
			out = append(out, entry)
		})
	}
	return out
}

// pruneBuildCache removes the least recently used entries that match until the
// cache holds at most keep bytes; cache mounts in use stay. The caller holds buildCacheMu.
func (b *engine) pruneBuildCache(keep int64, match func(lastUsed time.Time) bool) (deleted []string, reclaimed int64) {
	entries := b.buildCacheEntries()
	used := func(i int) time.Time {
		t, _ := time.Parse(time.RFC3339Nano, text(object(entries[i])["LastUsedAt"]))
		return t
	}
	sort.SliceStable(entries, func(i, j int) bool { return used(i).Before(used(j)) })
	total := int64(0)
	for _, e := range entries {
		total += integer(object(e)["Size"])
	}
	deleted = []string{}
	for i, e := range entries {
		if total <= keep {
			break
		}
		if size := integer(object(e)["Size"]); match(used(i)) && buildCacheInUse[str(object(e)["ID"])] == 0 {
			b.discard(filepath.Join(b.buildCacheDir(), str(object(e)["ID"])))
			deleted, reclaimed, total = append(deleted, str(object(e)["ID"])), reclaimed+size, total-size
		}
	}
	return deleted, reclaimed
}

// discard removes a cache entry: renamed away at once, deleted in the background.
func (b *engine) discard(dir string) {
	if !exists(dir) {
		return
	}
	trash, e := os.MkdirTemp(private(filepath.Join(b.storeDir(), "build")), "trash-")
	check(e)
	check(os.Rename(dir, filepath.Join(trash, "entry")))
	go os.RemoveAll(trash)
}
