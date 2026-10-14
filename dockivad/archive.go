package main

import (
	"archive/tar"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

// guestFile is a container path as the host sees it: under a mount's source
// when inside a mount, else under the container's root. Symlinks in the
// parent directories are resolved inside the container, as the guest would;
// the last element is not followed (docker cp copies a symlink as a symlink).
type guestFile struct {
	host, guest string // host path and the resolved guest path ("/a/b")
	readonly    bool
}

func (b *engine) guestFile(name, guest string) guestFile { return b.guestFiles(name)(guest, false) }

// guestFiles resolves many paths of one container; create makes missing parent
// directories (a tar extracted at "/" may reach into several mounts).
func (b *engine) guestFiles(name string) func(guest string, create bool) guestFile {
	p := b.container(name)
	c := readObject(filepath.Join(p, "config.json"))
	root := noLinks(p, "rootfs")
	rootLink := func(g string) (string, bool) { return readLink(filepath.Join(root, g)) }
	type mount struct {
		guest, source string
		readonly      bool
	}
	mounts := []mount{}
	for _, m := range savedMounts(c) {
		source := str(m["source"])
		if m["type"] == "volume" {
			source = volumeData(b.Home, source)
		}
		mounts = append(mounts, mount{resolveGuest(pathName(str(m["target"])[1:]), rootLink), source, m["readonly"] == true})
	}
	// at maps a resolved guest path to its base directory and the rest of the path.
	at := func(g string) (string, string, bool) {
		best := -1
		for i, m := range mounts {
			if (g == m.guest || strings.HasPrefix(g, m.guest+"/")) && (best < 0 || len(m.guest) > len(mounts[best].guest)) {
				best = i
			}
		}
		if best < 0 {
			return root, g, c["writable"] != true
		}
		return mounts[best].source, strings.TrimPrefix(strings.TrimPrefix(g, mounts[best].guest), "/"), mounts[best].readonly
	}
	link := func(g string) (string, bool) {
		base, rest, _ := at(g)
		return readLink(filepath.Join(base, rest))
	}
	return func(guest string, create bool) guestFile {
		reject(len(guest) <= 4096 && strings.HasPrefix(guest, "/"), "Path must be absolute: "+guest)
		clean := strings.TrimPrefix(path.Clean(guest), "/")
		dir, leaf := path.Split(clean)
		resolved := path.Join(resolveGuest(pathName(dir), link), leaf)
		base, rest, readonly := at(resolved)
		// Every directory above the file is now a real directory; noLinks re-checks that.
		// ponytail: a process in the container can swap a directory for a symlink between
		// this check and the copy; open with openat2-style beneath-checks if that matters.
		if rest == "" {
			return guestFile{base, "/" + resolved, readonly}
		}
		parent, last := path.Split(rest)
		if create && !readonly {
			return guestFile{filepath.Join(makeParents(base, parent), last), "/" + resolved, readonly}
		}
		return guestFile{filepath.Join(noLinksOrMissing(base, parent), last), "/" + resolved, readonly}
	}
}

func readLink(p string) (string, bool) {
	s, e := os.Lstat(p)
	if e != nil || s.Mode()&os.ModeSymlink == 0 {
		return "", false
	}
	target, e := os.Readlink(p)
	check(e)
	return target, true
}

// noLinksOrMissing is noLinks, answering 404 when a directory does not exist.
func noLinksOrMissing(root, relative string) string {
	p := filepath.Join(root, relative)
	if _, e := os.Lstat(p); errors.Is(e, fs.ErrNotExist) {
		panic(apiError{404, "No such file or directory"})
	}
	return noLinks(root, relative)
}

// makeParents is noLinks, creating missing directories as Docker does for a tar that
// leaves out parent entries (act sends "workflow/event.json" alone).
func makeParents(root, relative string) string {
	p := root
	for _, part := range strings.Split(relative, "/") {
		if part == "" || part == "." {
			continue
		}
		if e := os.Mkdir(filepath.Join(p, part), 0o755); !errors.Is(e, fs.ErrExist) {
			check(e)
		}
		p = noLinks(p, part)
	}
	return p
}

// pathStat is the X-Docker-Container-Path-Stat header docker cp reads.
func pathStat(f guestFile, requested string) string {
	st, e := os.Lstat(f.host)
	if errors.Is(e, fs.ErrNotExist) {
		panic(apiError{404, "Could not find the file " + requested + " in container"})
	}
	check(e)
	target := ""
	if st.Mode()&os.ModeSymlink != 0 {
		target, _ = readLink(f.host)
		if !strings.HasPrefix(target, "/") {
			target = path.Join(path.Dir(f.guest), target)
		}
	}
	raw, e := json.Marshal(obj{"name": path.Base(requested), "size": st.Size(), "mode": uint32(st.Mode()), "mtime": st.ModTime().UTC().Format(time.RFC3339Nano), "linkTarget": target})
	check(e)
	return base64.StdEncoding.EncodeToString(raw)
}

// archiveName is the name docker cp expects at the top of the tar: the requested
// base name, or "." for a path ending in "/." (copy a directory's contents).
func archiveName(requested string) string {
	if strings.HasSuffix(requested, "/.") || requested == "." {
		return "."
	}
	return path.Base(requested)
}

// getArchive streams a tar of a container path (docker cp CONTAINER:PATH -).
func (b *engine) getArchive(w http.ResponseWriter, name, requested string, head bool) {
	f := b.guestFile(name, requested)
	w.Header().Set("X-Docker-Container-Path-Stat", pathStat(f, requested))
	if head {
		w.WriteHeader(200)
		return
	}
	w.Header().Set("Content-Type", "application/x-tar")
	w.WriteHeader(200)
	out := tar.NewWriter(w)
	prefix := archiveName(requested)
	check(filepath.Walk(f.host, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, e := filepath.Rel(f.host, p)
		check(e)
		target := ""
		if info.Mode()&os.ModeSymlink != 0 {
			target, _ = readLink(p)
		}
		h, e := tar.FileInfoHeader(info, target)
		check(e)
		h.Name = prefix // "./x" keeps its "./": the docker CLI rebases on that prefix
		if rel != "." {
			h.Name += "/" + filepath.ToSlash(rel)
		}
		if info.IsDir() {
			h.Name += "/"
		}
		h.Uid, h.Gid, h.Uname, h.Gname = 0, 0, "", ""
		if !info.Mode().IsRegular() && !info.IsDir() && target == "" {
			return nil // sockets, FIFOs and devices are not copied
		}
		if e := out.WriteHeader(h); e != nil {
			return e
		}
		if info.Mode().IsRegular() {
			src, e := os.OpenFile(p, os.O_RDONLY|syscall.O_NOFOLLOW, 0)
			check(e)
			defer src.Close()
			_, e = io.CopyN(out, src, h.Size)
			return e
		}
		return nil
	}))
	check(out.Close())
}

// putArchive extracts a tar into a container directory (docker cp - CONTAINER:PATH).
func (b *engine) putArchive(name, requested string, body io.Reader, noOverwriteDirNonDir bool) {
	resolve := b.guestFiles(name)
	f := resolve(requested, false)
	st, e := os.Lstat(f.host)
	if errors.Is(e, fs.ErrNotExist) {
		panic(apiError{404, "Could not find the file " + requested + " in container"})
	}
	check(e)
	reject(st.IsDir(), "Extraction point is not a directory: "+requested)
	in := tar.NewReader(body)
	type dated struct {
		path string
		time time.Time
	}
	dirs := []dated{}
	for {
		h, e := in.Next()
		if e == io.EOF {
			break
		}
		check(e)
		rel := pathName(strings.TrimPrefix(h.Name, "./"))
		if rel == "" {
			continue // the directory itself
		}
		// Each entry finds its own mount: act extracts at "/" into a volume.
		to := resolve(path.Join(f.guest, rel), true)
		if to.readonly {
			panic(apiError{403, "Container path is read-only: " + to.guest})
		}
		dest := to.host
		old, e := os.Lstat(dest)
		exists := e == nil
		if exists && noOverwriteDirNonDir && old.IsDir() != (h.Typeflag == tar.TypeDir) {
			panic(apiError{400, "Cannot overwrite a directory with a non-directory (or the reverse): " + rel})
		}
		if exists && !(old.IsDir() && h.Typeflag == tar.TypeDir) {
			check(os.RemoveAll(dest))
		}
		mode := os.FileMode(h.Mode) & os.ModePerm
		switch h.Typeflag {
		case tar.TypeDir:
			if !exists || !old.IsDir() {
				check(os.Mkdir(dest, 0o700))
			}
			check(os.Chmod(dest, mode))
			dirs = append(dirs, dated{dest, h.ModTime})
			continue
		case tar.TypeReg:
			out, e := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, mode)
			check(e)
			_, e = io.Copy(out, in)
			check(errors.Join(e, out.Close()))
			check(os.Chmod(dest, mode))
		case tar.TypeSymlink:
			check(os.Symlink(linkTarget(h.Linkname), dest))
			continue
		case tar.TypeLink:
			source := resolve(path.Join(f.guest, pathName(h.Linkname)), false).host
			check(os.Link(source, dest))
		default:
			panic(apiError{400, "Unsupported archive entry type for " + rel})
		}
		check(os.Chtimes(dest, h.ModTime, h.ModTime))
	}
	for i := len(dirs) - 1; i >= 0; i-- { // after their contents, which change them
		check(os.Chtimes(dirs[i].path, dirs[i].time, dirs[i].time))
	}
}
