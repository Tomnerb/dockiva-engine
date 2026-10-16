package main

import (
	"errors"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
)

// Image layers (native/overlay.h). A container made with the layered layout keeps
// only its own files in <container>/rootfs, the upper layer, and runs over its
// image's unpacked files, the lower layer (config "lower"), which all containers of
// the image share and none writes: they stay warm in the Mac's file cache, and a
// container starts without copying the image. In an upper directory ".wh.NAME" hides
// NAME below (a whiteout) and ".wh..wh..opq" the whole lower directory (opaque), as
// in a layer tar. layers is the engine's view of a container's files as its guest
// sees them; rel paths are root-relative and link-free (resolveGuest them first).
// The engine changes only the upper layer, and never links to a lower file.
type layers struct{ upper, lower string } // lower "": a container with its own copy, or an image

const whiteoutPrefix, opaqueMarker = ".wh.", ".wh..wh..opq"

// containerLayers is container p's file system (config c).
func containerLayers(p string, c obj) layers {
	r := layers{upper: noLinks(p, "rootfs")}
	if l := text(c["lower"]); l != "" {
		r.lower = noLinks(filepath.Dir(l), filepath.Base(l))
	}
	return r
}

func relParts(rel string) []string {
	out := []string{}
	for _, part := range strings.Split(filepath.ToSlash(rel), "/") {
		reject(part != "..", "Path traversal rejected")
		if part != "" && part != "." {
			out = append(out, part)
		}
	}
	return out
}

// lookup is rel's upper entry and the lower entry beneath it, "" where there is
// none or a whiteout or an opaque directory above hides it. A symlink on the way
// finds nothing.
func (r layers) lookup(rel string) (up, low string) {
	u, l := r.upper, r.lower
	parts := relParts(rel)
	for i, part := range parts {
		if r.lower != "" && strings.HasPrefix(part, whiteoutPrefix) {
			return "", "" // reserved
		}
		up, low = "", ""
		var upper os.FileInfo
		if u != "" {
			if st, e := os.Lstat(filepath.Join(u, part)); e == nil {
				up, upper = filepath.Join(u, part), st
			}
		}
		if l != "" && (u == "" || !exists(filepath.Join(u, whiteoutPrefix+part))) && exists(filepath.Join(l, part)) {
			low = filepath.Join(l, part)
		}
		if i == len(parts)-1 {
			return up, low
		}
		if up != "" && !upper.IsDir() { // a file or link hides what is below
			return "", ""
		}
		if low != "" && (!isDir(low) || up != "" && exists(filepath.Join(up, opaqueMarker))) {
			low = ""
		}
		if up == "" && low == "" {
			return "", ""
		}
		u, l = up, low
	}
	return u, l
}

func isDir(p string) bool { st, e := os.Lstat(p); return e == nil && st.IsDir() }

// find is the host path showing rel, and whether rel exists: the upper entry, else
// the lower one.
func (r layers) find(rel string) (string, bool) {
	up, low := r.lookup(rel)
	if up != "" {
		return up, true
	}
	return low, low != ""
}

// link is rel's symlink target, for resolveGuest.
func (r layers) link(rel string) (string, bool) {
	p, ok := r.find(rel)
	if !ok {
		return "", false
	}
	return readLink(p)
}

// upperDir is directory rel in the upper layer, made where missing: from the lower
// directory (its mode and times) if the guest sees one, else (create) a new 0755
// one, which hides a deleted lower one; a file or symlink in the way is refused.
func (r layers) upperDir(rel string, create bool) string {
	p, sofar := r.upper, ""
	for _, part := range relParts(rel) {
		sofar = path.Join(sofar, part)
		next := filepath.Join(p, part)
		st, e := os.Lstat(next)
		if e == nil {
			reject(st.IsDir(), "Not a directory (a file or symlink is in the way): /"+sofar)
			p = next
			continue
		}
		if !errors.Is(e, fs.ErrNotExist) {
			check(e)
		}
		lower, seen := "", false
		if r.lower != "" {
			lower, seen = r.find(sofar)
		}
		var from os.FileInfo
		if seen {
			from, e = os.Lstat(lower)
			check(e)
			reject(from.IsDir(), "Not a directory (a file or symlink is in the way): /"+sofar)
		} else if !create {
			panic(apiError{404, "No such directory: /" + sofar})
		}
		if e := os.Mkdir(next, 0o700); e != nil && !errors.Is(e, fs.ErrExist) {
			check(e)
		}
		if from != nil {
			check(setMode(next, from.Mode()&permBits))
			_ = os.Chtimes(next, from.ModTime(), from.ModTime())
		} else {
			check(os.Chmod(next, 0o755))
			r.settle(sofar, true)
		}
		p = next
	}
	return p
}

// writable is the upper path at which to make rel (its directories made, as
// upperDir with create); settle after making it.
func (r layers) writable(rel string) string {
	dir, name := path.Split(path.Join(relParts(rel)...))
	reject(name != "", "Cannot replace the container root")
	reject(r.lower == "" || !strings.HasPrefix(name, whiteoutPrefix), "Reserved name: "+name)
	return filepath.Join(r.upperDir(dir, true), name)
}

// settle follows the making of upper entry rel: its whiteout goes, and a directory
// made where a lower one was deleted hides that one's files.
func (r layers) settle(rel string, directory bool) {
	if r.lower == "" {
		return
	}
	dir, name := path.Split(path.Join(relParts(rel)...))
	wh := filepath.Join(r.upper, dir, whiteoutPrefix+name)
	if !exists(wh) {
		return
	}
	if directory {
		f, e := os.OpenFile(filepath.Join(r.upper, dir, name, opaqueMarker), os.O_WRONLY|os.O_CREATE|syscall.O_NOFOLLOW, 0o600)
		check(e)
		f.Close()
	}
	check(os.Remove(wh))
}

// remove removes rel as the guest's rm -rf would: a whiteout hides the lower entry,
// and the upper one goes.
func (r layers) remove(rel string) {
	dir, name := path.Split(path.Join(relParts(rel)...))
	reject(name != "", "Cannot remove the container root")
	up, low := r.lookup(rel)
	if low != "" {
		f, e := os.OpenFile(filepath.Join(r.upperDir(dir, true), whiteoutPrefix+name), os.O_WRONLY|os.O_CREATE|syscall.O_NOFOLLOW, 0o600)
		check(e)
		f.Close()
	}
	if up != "" {
		check(os.RemoveAll(up))
	}
}

// copyUp makes a lower file rel the container's own (an APFS clone), so it may be
// changed or linked to; a file already in the upper layer stays.
func (r layers) copyUp(rel string) string {
	host, ok := r.find(rel)
	reject(ok, "No such file: /"+rel)
	up := filepath.Join(r.upper, filepath.FromSlash(path.Join(relParts(rel)...)))
	if host == up {
		return up
	}
	st, e := os.Lstat(host)
	check(e)
	reject(!st.IsDir(), "Cannot copy a directory up: /"+rel)
	dest := r.writable(rel)
	cloneTree(host, dest)
	if st.Mode().IsRegular() {
		check(setMode(dest, st.Mode()&permBits))
	}
	return dest
}

// guestEntry is one name of a merged directory listing.
type guestEntry struct {
	name, host string
	info       os.FileInfo
}

// readDir lists directory rel as the guest sees it, sorted by name.
func (r layers) readDir(rel string) []guestEntry {
	up, low := r.lookup(rel)
	reject(up != "" || low != "", "No such directory: /"+rel)
	out, seen := []guestEntry{}, map[string]bool{}
	add := func(dir string, upper bool) {
		entries, e := os.ReadDir(dir)
		check(e)
		for _, entry := range entries {
			name := entry.Name()
			if r.lower != "" && strings.HasPrefix(name, whiteoutPrefix) {
				if upper && name != opaqueMarker {
					seen[strings.TrimPrefix(name, whiteoutPrefix)] = true
				}
				continue
			}
			if seen[name] {
				continue
			}
			seen[name] = true
			info, e := entry.Info()
			check(e)
			out = append(out, guestEntry{name, filepath.Join(dir, name), info})
		}
	}
	if up != "" {
		add(up, true)
		if low != "" && isDir(low) && !exists(filepath.Join(up, opaqueMarker)) {
			add(low, false)
		}
	} else {
		add(low, false)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].name < out[j].name })
	return out
}

// walk visits rel and everything below it as the guest sees it, in lexical order
// (filepath.Walk's), with each entry's host path.
func (r layers) walk(rel string, fn func(rel, host string, info os.FileInfo) error) error {
	host, ok := r.find(rel)
	if !ok {
		return &fs.PathError{Op: "lstat", Path: "/" + rel, Err: fs.ErrNotExist}
	}
	info, e := os.Lstat(host)
	if e != nil {
		return e
	}
	return r.walkFrom(path.Join(relParts(rel)...), host, info, fn)
}

func (r layers) walkFrom(rel, host string, info os.FileInfo, fn func(rel, host string, info os.FileInfo) error) error {
	if e := fn(rel, host, info); e != nil || !info.IsDir() {
		if errors.Is(e, filepath.SkipDir) {
			return nil
		}
		return e
	}
	for _, entry := range r.readDir(rel) {
		if e := r.walkFrom(path.Join(rel, entry.name), entry.host, entry.info, fn); e != nil {
			return e
		}
	}
	return nil
}

// writeFile replaces a regular file (or a symlink, never followed) with content.
func (r layers) writeFile(rel, content string) {
	if host, ok := r.find(rel); ok {
		s, e := os.Lstat(host)
		check(e)
		reject(s.Mode().IsRegular() || s.Mode()&os.ModeSymlink != 0, "Cannot replace "+rel)
	}
	p := r.writable(rel)
	if _, e := os.Lstat(p); e == nil {
		check(os.Remove(p))
	}
	f, e := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, 0o644)
	check(e)
	defer f.Close()
	_, e = f.WriteString(content)
	check(e)
	r.settle(rel, false)
}

// mountPoint makes a link-free directory (or empty file) at guest, where missing.
func (r layers) mountPoint(guest string, directory bool) {
	parts := relParts(guest)
	for i := range parts {
		sofar := path.Join(parts[:i+1]...)
		last := i == len(parts)-1
		host, ok := r.find(sofar)
		if !ok {
			if last && !directory {
				p := r.writable(sofar)
				f, e := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, 0o644)
				check(e)
				f.Close()
				r.settle(sofar, false)
			} else {
				r.upperDir(sofar, true)
			}
			continue
		}
		st, e := os.Lstat(host)
		check(e)
		reject(st.Mode()&os.ModeSymlink == 0, "Mount point is a symlink: /"+guest)
		reject(!last && st.IsDir() || last && st.IsDir() == directory, "Mount point type does not match its source: /"+guest)
	}
}

// retainLowers keeps the unpacked files of image directory dir, which is going (docker
// rmi, a tag moved to another image), while containers use them as their lower layer:
// they move to <home>/lowers and the containers' configs follow.
func retainLowers(home, state, dir string) {
	root := filepath.Join(dir, "rootfs")
	users := containersUsing(state, root)
	if len(users) == 0 {
		return
	}
	kept := filepath.Join(private(filepath.Join(home, "lowers")), token()[:16])
	check(os.Rename(root, kept))
	for _, config := range users {
		c := readObject(config)
		c["lower"] = kept
		save(config, c)
	}
}

// containersUsing lists the config files of containers whose lower layer is root.
func containersUsing(state, root string) []string {
	entries, _ := os.ReadDir(state)
	out := []string{}
	for _, entry := range entries {
		config := filepath.Join(state, entry.Name(), "config.json")
		if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") || !exists(config) {
			continue
		}
		var lower string
		if safeRun(func() { lower = text(readObject(config)["lower"]) }) == nil && lower == root {
			out = append(out, config)
		}
	}
	return out
}

// releaseLower removes a retained lower layer (in <home>/lowers) no container uses.
func releaseLower(home, state, lower string) {
	if filepath.Dir(lower) != filepath.Join(home, "lowers") || len(containersUsing(state, lower)) > 0 {
		return
	}
	gone := filepath.Join(home, "lowers", ".removing-"+token()[:16])
	if os.Rename(lower, gone) == nil {
		go os.RemoveAll(gone)
	}
}
