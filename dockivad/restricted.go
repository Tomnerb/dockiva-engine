package main

// Guest trees hold what the engine's own user may not read or change: guest root's
// chmod 555 of a directory (Go's module cache is made that way) or chmod 000 of a
// directory or file is the host mode too, and the engine is that host user, not root.
// To remove, copy or archive such a tree it gives the directory or file the owner
// bits it lacks for as long as it needs them, and its mode back after: what is stored
// and exported keeps the modes the guest set. A running container sees the wider mode
// for that moment, as it does while the runtime does the same for guest root
// (native/fs.h, fs_dac).
//
// ponytail: a path given to docker cp or a mount that itself runs through a 000
// directory is still "not found" (layers.lookup); unlock on the way if it matters.

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"syscall"
)

// unlock gives directory or file p the owner permission bits of need it lacks, and
// returns what puts its mode back: defer unlock(dir, 0o700)() around making or
// removing entries of dir, 0o500 around reading it.
func unlock(p string, need fs.FileMode) func() {
	st, e := os.Lstat(p)
	if e != nil || st.Mode()&fs.ModeSymlink != 0 {
		return func() {}
	}
	return unlockAs(p, st, need)
}

func unlockAs(p string, st fs.FileInfo, need fs.FileMode) func() {
	mode := st.Mode() & permBits
	if mode&need == need || setMode(p, mode|need) != nil {
		return func() {}
	}
	return func() { _ = setMode(p, mode) }
}

// walkTree is filepath.WalkDir for a guest tree. A directory without owner read and
// search permission is readable from before fn sees it (with its own mode in d) until
// the walk has left it, also when fn panics.
func walkTree(root string, fn fs.WalkDirFunc) error {
	type held struct {
		prefix string
		lock   func()
	}
	var open []held
	leave := func(p string) { // lock again the directories p is not in
		for len(open) > 0 && !strings.HasPrefix(p, open[len(open)-1].prefix) {
			open[len(open)-1].lock()
			open = open[:len(open)-1]
		}
	}
	defer leave("")
	return filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		leave(p)
		if err != nil || !d.IsDir() {
			return fn(p, d, err)
		}
		info, e := d.Info()
		if e != nil {
			return fn(p, d, e)
		}
		if info.Mode().Perm()&0o500 != 0o500 {
			open = append(open, held{p + string(filepath.Separator), unlockAs(p, info, 0o500)})
		}
		return fn(p, fs.FileInfoToDirEntry(info), nil)
	})
}

// openGuest opens a guest's regular file to read it; one without read permission
// (Fedora's /etc/shadow is 000) has it for the open.
func openGuest(p string) (*os.File, error) {
	f, e := os.OpenFile(p, os.O_RDONLY|syscall.O_NOFOLLOW, 0)
	if !errors.Is(e, fs.ErrPermission) {
		return f, e
	}
	if st, e := os.Lstat(p); e != nil || !st.Mode().IsRegular() {
		return nil, &fs.PathError{Op: "open", Path: p, Err: fs.ErrPermission}
	}
	defer unlock(p, 0o400)()
	return os.OpenFile(p, os.O_RDONLY|syscall.O_NOFOLLOW, 0)
}

// through runs fn on rel of guest tree root; if a directory on the way has no search
// permission (fn answers EACCES), once more with them searchable. For a tree looked
// up beside the one being walked (a layer's base).
func through(root, rel string, fn func(p string) error) error {
	p := filepath.Join(root, rel)
	e := fn(p)
	if !errors.Is(e, fs.ErrPermission) {
		return e
	}
	dir := root
	for _, part := range strings.Split(filepath.Dir(rel), string(filepath.Separator)) {
		dir = filepath.Join(dir, part)
		defer unlock(dir, 0o100)()
	}
	return fn(p)
}

func lstatIn(root, rel string) (st os.FileInfo, e error) {
	e = through(root, rel, func(p string) error { st, e = os.Lstat(p); return e })
	return
}

func readlinkIn(root, rel string) (target string, e error) {
	e = through(root, rel, func(p string) error { target, e = os.Readlink(p); return e })
	return
}

// locked is an entry unlockTree made readable, with the mode it had.
type locked struct {
	rel  string
	mode fs.FileMode
}

// unlockTree makes every directory and regular file of a guest tree readable, for
// clonefile, which copies a tree whole or not at all; lockTree puts the modes back,
// in the tree or a copy of it.
func unlockTree(root string, held *[]locked) {
	check(filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		check(err)
		need := fs.FileMode(0o400)
		if d.IsDir() {
			need = 0o500
		} else if !d.Type().IsRegular() {
			return nil
		}
		info, e := d.Info()
		check(e)
		if mode := info.Mode() & permBits; mode&need != need {
			rel, e := filepath.Rel(root, p)
			check(e)
			check(setMode(p, mode|need)) // before WalkDir reads it
			*held = append(*held, locked{rel, mode})
		}
		return nil
	}))
}

func lockTree(root string, held []locked) {
	for i := len(held) - 1; i >= 0; i-- {
		_ = setMode(filepath.Join(root, held[i].rel), held[i].mode)
	}
}

// removeImageDir removes cached image directory dir, as docker rmi does. Its unpacked
// files stay for the containers running over them (retainLowers); the directory
// leaves the cache in one rename, so the image is listed whole or not at all, and what
// is left of a removal that fails goes at the next start (cleanupUnpublished).
func removeImageDir(store, state, dir string) {
	retainLowers(store, state, dir)
	gone := filepath.Join(filepath.Dir(dir), ".removing-"+token()[:16])
	check(os.Rename(dir, gone))
	if e := removeTree(gone); e != nil {
		fmt.Fprintln(os.Stderr, "dockivad: files of a removed image stay until the next start:", e)
	}
}
