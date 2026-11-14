package main

import (
	"archive/tar"
	"cmp"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"sort"
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
	files       *layers // the container's own files, unless in a mount: rel among them
	rel         string
}

func (b *engine) guestFile(name, guest string) guestFile { return b.guestFiles(name)(guest, false) }

// guestFiles resolves many paths of one container; create makes missing parent
// directories (a tar extracted at "/" may reach into several mounts).
func (b *engine) guestFiles(name string) func(guest string, create bool) guestFile {
	p := b.container(name)
	c := readObject(filepath.Join(p, "config.json"))
	view := containerView(p, c)
	files := view.files
	root := files.upper
	return func(guest string, create bool) guestFile {
		reject(len(guest) <= 4096 && strings.HasPrefix(guest, "/"), "Path must be absolute: "+guest)
		clean := strings.TrimPrefix(path.Clean(guest), "/")
		dir, leaf := path.Split(clean)
		resolved := path.Join(resolveGuest(pathName(dir), view.link), leaf)
		m, rest, mounted := view.at(resolved)
		if !mounted { // the container's own files, over its image's
			readonly := c["writable"] != true
			f := guestFile{filepath.Join(root, filepath.FromSlash(rest)), "/" + resolved, readonly, &files, rest}
			if host, ok := files.find(rest); ok {
				f.host = host
			} else if create && !readonly {
				files.upperDir(path.Dir(rest), true)
			} else if _, ok := files.find(path.Dir(rest)); !ok {
				panic(apiError{404, "No such file or directory"})
			}
			return f
		}
		base, readonly := m.source, m.readonly
		// Every directory above the file is now a real directory; noLinks re-checks that.
		// ponytail: a process in the container can swap a directory for a symlink between
		// this check and the copy; open with openat2-style beneath-checks if that matters.
		if rest == "" {
			return guestFile{base, "/" + resolved, readonly, nil, ""}
		}
		parent, last := path.Split(rest)
		if create && !readonly {
			return guestFile{filepath.Join(makeParents(base, parent), last), "/" + resolved, readonly, nil, ""}
		}
		return guestFile{filepath.Join(noLinksOrMissing(base, parent), last), "/" + resolved, readonly, nil, ""}
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
		if e := mkdirGuest(filepath.Join(p, part)); !errors.Is(e, fs.ErrExist) {
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
	b.upperSync(b.container(name))
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
	check(f.walk(func(p, rel string, info os.FileInfo) error {
		name := prefix // "./x" keeps its "./": the docker CLI rebases on that prefix
		if rel != "." {
			name += "/" + filepath.ToSlash(rel)
		}
		return tarEntry(out, name, p, info)
	}))
	check(out.Close())
}

// tarEntry writes host file p (lstat info) into a tar as name, owned by root. Sockets,
// FIFOs and devices are left out.
func tarEntry(out *tar.Writer, name, p string, info os.FileInfo) error {
	target := ""
	if info.Mode()&os.ModeSymlink != 0 {
		target, _ = readLink(p)
	}
	h, e := tar.FileInfoHeader(info, target)
	check(e)
	h.Name = name
	if info.IsDir() {
		h.Name += "/"
	}
	h.Uid, h.Gid, h.Uname, h.Gname = 0, 0, "", ""
	if !info.Mode().IsRegular() && !info.IsDir() && target == "" {
		return nil
	}
	if e := out.WriteHeader(h); e != nil {
		return e
	}
	if info.Mode().IsRegular() {
		src, e := openGuest(p)
		check(e)
		defer src.Close()
		_, e = io.CopyN(out, src, h.Size)
		return e
	}
	return nil
}

// export is docker export: the container's files as its guest sees them, as a tar
// (mounts left out, as Docker does).
func (b *engine) export(w http.ResponseWriter, name string) {
	p := b.container(name)
	b.upperSync(p)
	r := containerLayers(p, readObject(filepath.Join(p, "config.json")))
	w.Header().Set("Content-Type", "application/x-tar")
	w.WriteHeader(200)
	out := tar.NewWriter(w)
	check(r.walk("", func(rel, host string, info os.FileInfo) error {
		if rel == "" {
			return nil
		}
		return tarEntry(out, rel, host, info)
	}))
	check(out.Close())
}

// engineFiles are what the engine writes into every container (Docker bind-mounts
// them) and the mount points it makes (.dockiva: FEX's, x86.go): docker diff and
// docker commit leave them out.
var engineFiles = map[string]bool{"etc/hosts": true, "etc/resolv.conf": true, "etc/hostname": true, "dev": true, "proc": true, "sys": true, ".dockiva": true}

// changes is docker diff: a layered container's own files (its upper layer) against
// its image's: Kind 1 added, 0 changed, 2 deleted (a whiteout). A directory of the
// image shows as changed when something beneath it did, or it was emptied (opaque);
// one the engine made (for its files, or a mount point) only for what is in it. An
// amd64 container's FEX files (x86.go) are left out, as docker commit leaves them out.
func (b *engine) changes(name string) []obj {
	p := b.container(name)
	b.upperSync(p)
	c := readObject(filepath.Join(p, "config.json"))
	reject(c["lower"] != nil, "docker diff needs the layered layout (this container was made with --rootfs copy)")
	r := containerLayers(p, c)
	image := layers{upper: r.lower}
	out, quiet := []obj{}, map[string]bool{"etc": true, "tmp": os.Getenv("DOCKIVA_TMP") == "memory"}
	for _, m := range savedMounts(c) {
		quiet[pathName(mountTarget(str(m["target"]))[1:])] = true
	}
	x86, fex := c["arch"] == "amd64", map[string]bool{}
	if x86 {
		for _, g := range missingIn(r.lower, fexState(r, stringsOfAny(c["env"]))) {
			if rel := strings.TrimPrefix(g, "/"); path.Base(rel) == "fex-emu" {
				fex[rel] = true
			} else {
				quiet[rel] = true // a parent FEX made: shown for the program's own files in it
			}
		}
	}
	var visit func(rel string) bool
	visit = func(rel string) bool {
		dir := filepath.Join(r.upper, filepath.FromSlash(rel))
		defer unlock(dir, 0o500)()
		entries, e := os.ReadDir(dir)
		check(e)
		changed := false
		for _, entry := range entries {
			name, child := entry.Name(), path.Join(rel, entry.Name())
			switch {
			case name == opaqueMarker || strings.HasPrefix(name, whiteoutPrefix+whiteoutPrefix):
			case strings.HasPrefix(name, whiteoutPrefix):
				out, changed = append(out, obj{"Path": "/" + path.Join(rel, strings.TrimPrefix(name, whiteoutPrefix)), "Kind": 2}), true
			case engineFiles[child] || fex[child]:
			case x86 && rel == "tmp" && strings.HasSuffix(name, fexSocket) && entry.Type()&fs.ModeSocket != 0:
			default:
				_, old := image.find(child)
				below := entry.IsDir() && visit(child)
				if !old && !(entry.IsDir() && quiet[child]) || !entry.IsDir() || below || exists(filepath.Join(r.upper, filepath.FromSlash(child), opaqueMarker)) {
					kind := 1
					if old {
						kind = 0
					}
					out, changed = append(out, obj{"Path": "/" + child, "Kind": kind}), true
				}
			}
		}
		return changed
	}
	visit("")
	sort.Slice(out, func(i, j int) bool { return str(out[i]["Path"]) < str(out[j]["Path"]) })
	return out
}

// putArchive extracts a tar into a container directory (docker cp - CONTAINER:PATH).
func (b *engine) putArchive(name, requested string, body io.Reader, noOverwriteDirNonDir bool) {
	b.upperSync(b.container(name)) // the run's files first: what is copied in replaces them on disk
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
			if to.files != nil {
				to.files.remove(to.rel)
			} else {
				defer unlock(filepath.Dir(dest), 0o700)()
				check(removeTree(dest))
			}
			exists = false
		}
		if to.files != nil { // made in the upper layer
			if exists {
				dest = to.files.upperDir(to.rel, false)
			} else {
				dest = to.files.writable(to.rel)
			}
			defer to.files.settle(to.rel, h.Typeflag == tar.TypeDir)
		}
		// Docker's root writes into a directory without write permission: it has it until
		// the archive is in (restricted.go).
		defer unlock(filepath.Dir(dest), 0o700)()
		mode := tarMode(h)
		switch h.Typeflag {
		case tar.TypeDir:
			if !exists || !old.IsDir() {
				check(os.Mkdir(dest, 0o700))
			}
			check(setMode(dest, mode))
			dirs = append(dirs, dated{dest, h.ModTime})
			continue
		case tar.TypeReg:
			out, e := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, mode&os.ModePerm)
			check(e)
			_, e = io.Copy(out, in)
			check(errors.Join(e, out.Close()))
			check(setMode(dest, mode))
		case tar.TypeSymlink:
			check(os.Symlink(linkTarget(h.Linkname), dest))
			continue
		case tar.TypeLink:
			from := resolve(path.Join(f.guest, pathName(h.Linkname)), false)
			source := from.host
			if from.files != nil {
				source = from.files.copyUp(from.rel) // never a link to the image's file
			}
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

// walk visits f and everything below it, as the container sees it: each entry's host
// path, its path relative to f (".") and its lstat.
func (f guestFile) walk(fn func(host, rel string, info os.FileInfo) error) error {
	if f.files != nil {
		base := path.Join(relParts(f.rel)...)
		return f.files.walk(f.rel, func(rel, host string, info os.FileInfo) error {
			return fn(host, cmp.Or(strings.TrimPrefix(strings.TrimPrefix(rel, base), "/"), "."), info)
		})
	}
	return filepath.Walk(f.host, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, e := filepath.Rel(f.host, p)
		check(e)
		return fn(p, filepath.ToSlash(rel), info)
	})
}
