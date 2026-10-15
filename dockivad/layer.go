package main

import (
	"archive/tar"
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

// writeDiffLayer writes into blobs (an OCI layout's blobs/sha256 directory) an
// uncompressed tar layer holding how root differs from base: entries that are new
// or changed in root, and whiteouts (".wh.NAME") for entries of base missing from
// root. base "" means an empty base (every entry of root is new). It returns the
// layer's digest ("sha256:<hex>", which is also its DiffID since it is
// uncompressed), its size in bytes, and whether anything differs.
//
// Files compare by type, mode, size and nanosecond mtime, directories by type and
// mode, symlinks by type, mode and target. The same trees give the same digest.
//
// ponytail: hard links within root are written as separate files (their bytes
// twice), and sockets, FIFOs and devices are left out (the runtime serves /dev).
func writeDiffLayer(root, base, blobs string) (digest string, size int64, changed bool) {
	f, e := os.CreateTemp(blobs, ".layer-")
	check(e)
	defer os.Remove(f.Name()) // on failure; renamed away on success
	defer f.Close()
	hash, n := sha256.New(), &counter{}
	buf := bufio.NewWriterSize(io.MultiWriter(f, hash, n), 1<<20)
	out := tar.NewWriter(buf)
	add := func(h *tar.Header) {
		// As Docker writes layers: PAX, whole seconds, no owners (uid/gid 0).
		h.Format, h.ModTime = tar.FormatPAX, h.ModTime.Truncate(time.Second)
		check(out.WriteHeader(h))
		changed = true
	}
	link := func(p string) string { target, e := os.Readlink(p); check(e); return target }
	if base != "" {
		// Whiteouts first. A directory root lacks, or holds as another type, is not
		// descended: its whiteout or replacement removes it whole.
		check(filepath.WalkDir(base, func(p string, d fs.DirEntry, err error) error {
			check(err)
			rel, e := filepath.Rel(base, p)
			check(e)
			if rel == "." {
				return nil
			}
			st, e := os.Lstat(filepath.Join(root, rel))
			gone := os.IsNotExist(e)
			if gone {
				dir, name := filepath.Split(rel)
				add(&tar.Header{Name: dir + ".wh." + name, Typeflag: tar.TypeReg, ModTime: time.Unix(0, 0)})
			} else {
				check(e)
			}
			if d.IsDir() && (gone || !st.IsDir()) {
				return filepath.SkipDir
			}
			return nil
		}))
	}
	// fresh is a directory base lacks (or had as another type): all beneath it is new,
	// and base is not looked up there (its path may run through a file or symlink).
	fresh := ""
	check(filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		check(err)
		rel, e := filepath.Rel(root, p)
		check(e)
		info, e := d.Info()
		check(e)
		kind := info.Mode().Type()
		if rel == "." || kind&^(fs.ModeDir|fs.ModeSymlink) != 0 {
			return nil // the root itself; sockets, FIFOs and devices
		}
		if base != "" && !strings.HasPrefix(rel, fresh+"/") {
			was := filepath.Join(base, rel)
			old, e := os.Lstat(was)
			if e == nil && old.Mode() == info.Mode() &&
				(kind == fs.ModeDir ||
					kind == fs.ModeSymlink && link(was) == link(p) ||
					kind == 0 && old.Size() == info.Size() && old.ModTime().Equal(info.ModTime())) {
				return nil
			}
			if e != nil && !os.IsNotExist(e) {
				check(e)
			}
			if kind == fs.ModeDir && (e != nil || !old.IsDir()) {
				fresh = rel
			}
		}
		h := &tar.Header{Name: rel, Typeflag: tar.TypeReg, Mode: int64(info.Sys().(*syscall.Stat_t).Mode & 0o7777), ModTime: info.ModTime()}
		switch kind {
		case fs.ModeDir:
			h.Name, h.Typeflag = rel+"/", tar.TypeDir
		case fs.ModeSymlink:
			h.Typeflag, h.Linkname = tar.TypeSymlink, link(p)
		default:
			h.Size = info.Size()
		}
		add(h)
		if kind == 0 {
			in, e := os.OpenFile(p, os.O_RDONLY|syscall.O_NOFOLLOW, 0)
			check(e)
			defer in.Close()
			_, e = io.CopyN(out, in, h.Size)
			check(e)
		}
		return nil
	}))
	check(out.Close())
	check(buf.Flush())
	check(barrierSync(f)) // the bytes before the name: a crash never leaves a torn blob
	check(f.Close())
	digest = "sha256:" + hex.EncodeToString(hash.Sum(nil))
	if p := filepath.Join(blobs, digest[7:]); !exists(p) {
		check(os.Rename(f.Name(), p))
	}
	return digest, n.n, changed
}

// imageDocs reads an OCI layout's image manifest and image config (the single
// manifest index.json lists).
func imageDocs(layout string) (manifest, config obj) {
	items := array(readObject(filepath.Join(layout, "index.json"))["manifests"])
	reject(len(items) == 1, "Expected exactly one image")
	manifest = decode(blob(layout, object(items[0]), maxJSON, manifestMedia, dockerManifest), maxJSON)
	config = decode(blob(layout, object(manifest["config"]), maxJSON, configMedia, dockerConfig), maxJSON)
	return manifest, config
}

// treeSize is the total size of the regular files under root (docker images SIZE).
func treeSize(root string) int64 {
	var n int64
	check(filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		check(err)
		if d.Type().IsRegular() {
			info, e := d.Info()
			check(e)
			n += info.Size()
		}
		return nil
	}))
	return n
}
