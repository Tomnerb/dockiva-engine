package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"syscall"
)

const manifestMedia = "application/vnd.oci.image.manifest.v1+json"
const configMedia = "application/vnd.oci.image.config.v1+json"
const layerMedia = "application/vnd.oci.image.layer.v1.tar"
const indexMedia = "application/vnd.oci.image.index.v1+json"

// ponytail: images are pulled, unpacked and copied in memory, so these caps bound
// the engine's memory; stream layers to disk to lift them further.
const maxBlob = int64(256 << 20)
const maxPacked = int64(512 << 20)
const maxMembers = 65536
const maxJSON = int64(1 << 20)
const maxExpanded = int64(1 << 30)

var digestPattern = regexp.MustCompile(`^sha256:[0-9a-f]{64}$`)

type entry struct {
	Dir, Executable bool
	Data            []byte
	Link            string // symlink target, resolved inside the guest root
	Hardlink        string // tree path of the file this one shares data with
}
type image struct {
	Digest           string
	Entry, Args, Env []string
	WorkingDir       string // absolute guest path; "/" when the image sets none
	StopSignal       string // signal name sent on stop: SIGTERM unless the image sets one
	Tree             map[string]entry
	Blobs            map[string][]byte
	Descriptor       obj
}

func pathName(name string) string {
	reject(len(name) <= 1024 && !strings.HasPrefix(name, "/") && !strings.ContainsAny(name, "\\:"), "Invalid image path")
	for _, c := range name {
		reject(c >= 32 && c < 127, "Only printable ASCII paths supported")
	}
	out := []string{}
	for _, s := range strings.Split(name, "/") {
		reject(s != "..", "Path traversal rejected")
		if s != "" && s != "." {
			out = append(out, s)
		}
	}
	reject(len(out) <= 32, "Path too deep")
	return strings.Join(out, "/")
}
func blob(root string, desc obj, limit int64, media ...string) []byte {
	d := text(desc["digest"])
	reject(digestPattern.MatchString(d), "Invalid sha256 descriptor")
	ok := false
	for _, m := range media {
		ok = ok || desc["mediaType"] == m
	}
	reject(ok, "Unsupported descriptor media type")
	_, embedded := desc["data"]
	reject(idle(desc["urls"]) && !embedded, "External/embedded blobs unsupported")
	size := integer(desc["size"])
	reject(size >= 0 && size <= limit, "Invalid descriptor size")
	b := readBounded(root, "blobs/sha256/"+d[7:], limit)
	reject(int64(len(b)) == size && digest(b) == d, "Blob size/digest mismatch")
	return b
}

// resolveGuest follows guest symlinks the way native/fs.h does: absolute targets
// restart at the guest root, ".." stops at the root, and at most 40 links are
// followed. link reports a path's symlink target. The result is root-relative.
func resolveGuest(guest string, link func(string) (string, bool)) string {
	rest, cur, hops := strings.Split(guest, "/"), []string{}, 0
	for len(rest) > 0 {
		part := rest[0]
		rest = rest[1:]
		switch part {
		case "", ".":
			continue
		case "..":
			if len(cur) > 0 {
				cur = cur[:len(cur)-1]
			}
			continue
		}
		p := strings.Join(append(cur[:len(cur):len(cur)], part), "/")
		if target, ok := link(p); ok {
			hops++
			reject(hops <= 40, "Too many symbolic links: "+guest)
			if strings.HasPrefix(target, "/") {
				cur = cur[:0]
			}
			rest = append(strings.Split(target, "/"), rest...)
			continue
		}
		cur = append(cur, part)
	}
	return strings.Join(cur, "/")
}
func linkTarget(name string) string {
	reject(name != "" && len(name) <= 1024 && !strings.ContainsAny(name, "\\:"), "Invalid symlink target")
	for _, c := range name {
		reject(c >= 32 && c < 127, "Only printable ASCII symlink targets supported")
	}
	return name
}
func removeTree(tree map[string]entry, name string) {
	for k := range tree {
		if name == "" || k == name || strings.HasPrefix(k, name+"/") {
			delete(tree, k)
		}
	}
}
func applyLayer(tree map[string]entry, raw []byte, members *int) {
	type named struct {
		Name string
		E    entry
	}
	entries := []named{}
	names := map[string]bool{}
	r := tar.NewReader(bytes.NewReader(raw))
	for {
		h, e := r.Next()
		if e == io.EOF {
			break
		}
		check(e)
		*members++
		reject(*members <= maxMembers, "Too many tar members")
		name := pathName(h.Name)
		dir := h.Typeflag == tar.TypeDir
		symlink, hardlink := h.Typeflag == tar.TypeSymlink, h.Typeflag == tar.TypeLink
		reject(dir || symlink || hardlink || h.Typeflag == tar.TypeReg || h.Typeflag == tar.TypeRegA, "Devices/FIFOs unsupported")
		for k := range h.PAXRecords {
			reject(!strings.HasPrefix(k, "GNU.sparse"), "Sparse files unsupported")
		}
		reject(h.Size >= 0 && h.Size <= maxBlob, "Layer file exceeds limit")
		if name == "" {
			reject(dir, "Empty file path")
			continue
		}
		reject(!names[name], "Duplicate path within layer")
		names[name] = true
		reject(!dir || h.Size == 0, "Directory content unsupported")
		if symlink || hardlink {
			reject(h.Size == 0, "Link content unsupported")
			if symlink {
				entries = append(entries, named{name, entry{Link: linkTarget(h.Linkname)}})
			} else {
				target := pathName(h.Linkname)
				reject(target != "" && target != name, "Invalid hardlink target")
				entries = append(entries, named{name, entry{Hardlink: target}})
			}
			continue
		}
		b, e := io.ReadAll(io.LimitReader(r, maxBlob+1))
		check(e)
		reject(int64(len(b)) == h.Size, "Truncated tar content")
		entries = append(entries, named{name, entry{Dir: dir, Executable: h.Mode&0111 != 0, Data: b}})
	}
	for _, v := range entries {
		base := filepath.Base(v.Name)
		if !strings.HasPrefix(base, ".wh.") {
			continue
		}
		reject(!v.E.Dir && len(v.E.Data) == 0 && v.E.Link == "" && v.E.Hardlink == "", "Invalid whiteout")
		parent := filepath.ToSlash(filepath.Dir(v.Name))
		if parent == "." {
			parent = ""
		}
		if base == ".wh..wh..opq" {
			p, ok := tree[parent]
			reject(!ok || p.Dir, "Invalid opaque parent")
			for k := range tree {
				if parent == "" || strings.HasPrefix(k, parent+"/") {
					delete(tree, k)
				}
			}
		} else {
			target := strings.TrimPrefix(base, ".wh.")
			reject(target != "" && target != "." && target != ".." && !strings.HasPrefix(target, ".wh."), "Invalid whiteout target")
			if parent != "" {
				target = parent + "/" + target
			}
			removeTree(tree, target)
		}
	}
	sort.SliceStable(entries, func(i, j int) bool {
		if entries[i].E.Dir != entries[j].E.Dir {
			return entries[i].E.Dir
		}
		return strings.Count(entries[i].Name, "/") < strings.Count(entries[j].Name, "/")
	})
	place := func(name string, e entry) {
		parts := strings.Split(name, "/")
		for n := 1; n < len(parts); n++ {
			p := strings.Join(parts[:n], "/")
			old, ok := tree[p]
			// Also keeps extraction from writing through a symlinked directory.
			reject(!ok || old.Dir, "File or symlink used as directory")
			if !ok {
				tree[p] = entry{Dir: true}
			}
		}
		old, ok := tree[name]
		if !e.Dir || (ok && old.Dir != e.Dir) {
			removeTree(tree, name)
		}
		tree[name] = e
	}
	for _, v := range entries {
		if !strings.HasPrefix(filepath.Base(v.Name), ".wh.") && v.E.Hardlink == "" {
			place(v.Name, v.E)
		}
	}
	// Hardlinks refer to files from this or earlier layers; they share data in
	// memory and become host hardlinks on extraction.
	for _, v := range entries {
		if v.E.Hardlink == "" {
			continue
		}
		src, ok := tree[v.E.Hardlink]
		reject(ok && !src.Dir && src.Link == "", "Hardlink target missing or not a regular file")
		origin := v.E.Hardlink
		if src.Hardlink != "" {
			origin = src.Hardlink
		}
		place(v.Name, entry{Executable: src.Executable, Data: src.Data, Hardlink: origin})
	}
	reject(len(tree) <= 4096, "Too many extracted paths")
	fold := map[string]bool{}
	for k := range tree {
		low := strings.ToLower(k)
		reject(!fold[low], "Case-colliding paths unsupported")
		fold[low] = true
	}
}
func loadImage(root string) *image {
	header := readObject(filepath.Join(root, "oci-layout"))
	reject(header["imageLayoutVersion"] == "1.0.0", "Unsupported OCI version")
	index := readObject(filepath.Join(root, "index.json"))
	reject(integer(index["schemaVersion"]) == 2, "Invalid index version")
	items := array(index["manifests"])
	reject(len(items) == 1, "Expected exactly one image")
	desc := object(items[0])
	raw := blob(root, desc, maxJSON, manifestMedia)
	manifest := decode(raw, maxJSON)
	reject(integer(manifest["schemaVersion"]) == 2 && manifest["mediaType"] == manifestMedia, "Invalid image manifest")
	if p := desc["platform"]; p != nil {
		m := object(p)
		reject(m["os"] == "linux" && m["architecture"] == "arm64", "Only linux/arm64 supported")
	}
	im := &image{Digest: str(desc["digest"]), Descriptor: desc, Tree: map[string]entry{}, Blobs: map[string][]byte{str(desc["digest"]): raw}}
	cd := object(manifest["config"])
	cr := blob(root, cd, maxJSON, configMedia)
	im.Blobs[str(cd["digest"])] = cr
	c := decode(cr, maxJSON)
	reject(c["os"] == "linux" && c["architecture"] == "arm64", "Only linux/arm64 supported")
	settings := object(c["config"])
	reject(text(settings["User"]) == "", "User unsupported")
	im.StopSignal = stopSignal(text(settings["StopSignal"]))
	im.Entry = stringsOf(settings["Entrypoint"])
	im.Args = stringsOf(settings["Cmd"])
	im.Env = stringsOf(settings["Env"])
	validateEnv(im.Env)
	layers := array(manifest["layers"])
	reject(len(layers) > 0 && len(layers) <= 32, "Expected 1..32 layers")
	rf := object(c["rootfs"])
	reject(rf["type"] == "layers", "Invalid rootfs")
	diffs := array(rf["diff_ids"])
	reject(len(layers) == len(diffs), "Layer/DiffID count mismatch")
	var total int64
	packedTotal := int64(len(raw) + len(cr))
	members := 0
	for i, d := range layers {
		ld := object(d)
		packedTotal += integer(ld["size"])
		reject(packedTotal <= maxPacked, "Packed image exceeds 512 MiB")
		packed := blob(root, ld, maxBlob, layerMedia, layerMedia+"+gzip")
		im.Blobs[str(ld["digest"])] = packed
		raw := packed
		if ld["mediaType"] == layerMedia+"+gzip" {
			r, e := gzip.NewReader(bytes.NewReader(packed))
			check(e)
			raw, e = io.ReadAll(io.LimitReader(r, maxExpanded-total+1))
			check(e)
			check(r.Close())
		}
		total += int64(len(raw))
		reject(total <= maxExpanded, "Expanded layers exceed 1 GiB")
		reject(digest(raw) == str(diffs[i]), "Layer DiffID mismatch")
		applyLayer(im.Tree, raw, &members)
	}
	// Docker creates WorkingDir if missing. Declared volumes become ordinary
	// directories in the private root until volume support exists.
	im.WorkingDir = "/"
	if wd := text(settings["WorkingDir"]); wd != "" {
		im.WorkingDir = im.ensureDir(wd)
	}
	for v := range object(settings["Volumes"]) {
		im.ensureDir(v)
	}
	im.command(nil)
	return im
}

// ensureDir makes an absolute guest directory exist in the tree, following
// guest symlinks like the runtime does, and returns the path as given.
func (im *image) ensureDir(guest string) string {
	reject(strings.HasPrefix(guest, "/"), "Directory must be an absolute guest path: "+guest)
	p := resolveGuest(pathName(guest[1:]), func(p string) (string, bool) {
		e := im.Tree[p]
		return e.Link, e.Link != ""
	})
	if p == "" {
		return guest
	}
	parts := strings.Split(p, "/")
	for n := 1; n <= len(parts); n++ {
		q := strings.Join(parts[:n], "/")
		e, ok := im.Tree[q]
		reject(!ok || e.Dir, "Not a directory in the image: "+guest)
		if !ok {
			im.Tree[q] = entry{Dir: true}
		}
	}
	return guest
}
func validateEnv(env []string) {
	reject(len(env) <= 64, "Too many environment values")
	seen := map[string]bool{}
	for _, v := range env {
		k, _, ok := strings.Cut(v, "=")
		reject(ok && k != "" && !strings.ContainsRune(v, 0) && !seen[k], "Invalid/duplicate environment key")
		seen[k] = true
	}
}
func (im *image) command(override []string) []string {
	args := im.Args
	if override != nil {
		args = override
	}
	cmd := append(append([]string{}, im.Entry...), args...)
	reject(len(cmd) > 0 && len(cmd) <= 64, "Expected executable and at most 63 arguments")
	find := func(guest string) (entry, bool) {
		v, ok := im.Tree[resolveGuest(pathName(guest), func(p string) (string, bool) {
			e := im.Tree[p]
			return e.Link, e.Link != ""
		})]
		return v, ok && !v.Dir && v.Executable
	}
	if !strings.Contains(cmd[0], "/") {
		// Like runc, look a bare command up in the image PATH (Docker's default when unset).
		path := "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
		for _, kv := range im.Env {
			if v, ok := strings.CutPrefix(kv, "PATH="); ok {
				path = v
			}
		}
		for _, dir := range strings.Split(path, ":") {
			if candidate := strings.TrimSuffix(dir, "/") + "/" + cmd[0]; strings.HasPrefix(candidate, "/") {
				if _, ok := find(candidate[1:]); ok {
					cmd[0] = candidate
					break
				}
			}
		}
	}
	reject(strings.HasPrefix(cmd[0], "/"), "Executable not found in PATH: "+cmd[0])
	v, ok := find(cmd[0][1:])
	reject(ok, "Executable missing or not executable")
	if bytes.HasPrefix(v.Data, []byte("#!")) {
		// Like Linux execve: "#!INTERPRETER [ONE-ARGUMENT]" runs INTERPRETER [ARG] SCRIPT ARGS...
		line := v.Data[2:]
		if i := bytes.IndexByte(line, '\n'); i >= 0 {
			line = line[:i]
		}
		reject(len(line) <= 255, "Script interpreter line too long")
		fields := strings.TrimSpace(strings.TrimSuffix(string(line), "\r"))
		interpreter, argument, _ := strings.Cut(fields, " ")
		interpreter, argument = strings.TrimSpace(interpreter), strings.TrimSpace(argument)
		reject(strings.HasPrefix(interpreter, "/"), "Script interpreter must be an absolute path")
		script := append([]string{interpreter}, cmd[0])
		if argument != "" {
			script = []string{interpreter, argument, cmd[0]}
		}
		cmd = append(script, cmd[1:]...)
		reject(len(cmd) <= 64, "Expected executable and at most 63 arguments")
		v, ok = find(interpreter[1:])
		reject(ok, "Script interpreter missing or not executable: "+interpreter)
	}
	reject(bytes.HasPrefix(v.Data, []byte{127, 'E', 'L', 'F', 2, 1, 1}), "Expected little-endian ELF64")
	total := 0
	for _, s := range append(append([]string{}, cmd...), im.Env...) {
		reject(!strings.ContainsRune(s, 0), "NUL in process string")
		total += len(s) + 1
	}
	reject(total <= 1<<20, "Process strings exceed 1 MiB") // the runtime's exec limit
	return cmd
}
func (im *image) extract(root string) {
	s, e := os.Lstat(root)
	check(e)
	reject(s.IsDir() && s.Mode()&os.ModeSymlink == 0, "Invalid extraction root")
	entries, e := os.ReadDir(root)
	check(e)
	reject(len(entries) == 0, "Extraction requires empty directory")
	names := []string{}
	for k := range im.Tree {
		names = append(names, k)
	}
	sort.Slice(names, func(i, j int) bool { return strings.Count(names[i], "/") < strings.Count(names[j], "/") })
	// The tree never places a path under a symlink (see place in applyLayer), so
	// these host operations cannot traverse a guest link.
	hardlinks := []string{}
	for _, name := range names {
		v := im.Tree[name]
		p := filepath.Join(root, name)
		if v.Dir {
			check(os.MkdirAll(p, 0700))
			continue
		}
		check(os.MkdirAll(filepath.Dir(p), 0700))
		if v.Link != "" {
			check(os.Symlink(v.Link, p))
			continue
		}
		if origin, ok := im.Tree[v.Hardlink]; ok && v.Hardlink != "" && origin.Hardlink == "" && len(v.Data) > 0 && len(origin.Data) == len(v.Data) && &origin.Data[0] == &v.Data[0] {
			hardlinks = append(hardlinks, name)
			continue
		}
		mode := os.FileMode(0600)
		if v.Executable {
			mode = 0700
		}
		f, e := os.OpenFile(p, os.O_CREATE|os.O_EXCL|os.O_WRONLY, mode)
		check(e)
		_, e = f.Write(v.Data)
		f.Close()
		check(e)
	}
	for _, name := range hardlinks {
		check(os.Link(filepath.Join(root, im.Tree[name].Hardlink), filepath.Join(root, name)))
	}
	// The runtime serves /dev/null, /dev/zero and /dev/urandom beneath /dev.
	if _, ok := im.Tree["dev"]; !ok {
		check(os.Mkdir(filepath.Join(root, "dev"), 0700))
	}
}
func commitImage(cache, name, layout string) string {
	cache = private(cache)
	im := loadImage(layout)
	target := filepath.Join(cache, key(name))
	if exists(target) {
		old := loadImage(noLinks(target, "layout"))
		reject(old.Digest == im.Digest, "Cached name already has another digest")
		return filepath.Join(target, "layout")
	}
	stage, e := os.MkdirTemp(cache, ".incoming-")
	check(e)
	defer os.RemoveAll(stage)
	dest := filepath.Join(stage, "layout")
	check(os.MkdirAll(filepath.Join(dest, "blobs/sha256"), 0700))
	for d, b := range im.Blobs {
		check(os.WriteFile(filepath.Join(dest, "blobs/sha256", d[7:]), b, 0600))
	}
	save(filepath.Join(dest, "oci-layout"), obj{"imageLayoutVersion": "1.0.0"})
	save(filepath.Join(dest, "index.json"), obj{"schemaVersion": 2, "manifests": []any{im.Descriptor}})
	save(filepath.Join(stage, "image.json"), obj{"name": name, "digest": im.Digest})
	check(os.Rename(stage, target))
	return filepath.Join(target, "layout")
}

// stopSignal accepts the signals a container may ask to be stopped with.
func stopSignal(name string) string {
	if name == "" {
		return "SIGTERM"
	}
	if !strings.HasPrefix(name, "SIG") {
		name = "SIG" + name
	}
	_, ok := stopSignals[name]
	reject(ok, "StopSignal unsupported: "+name)
	return name
}

var stopSignals = map[string]syscall.Signal{"SIGTERM": syscall.SIGTERM, "SIGINT": syscall.SIGINT, "SIGQUIT": syscall.SIGQUIT, "SIGHUP": syscall.SIGHUP, "SIGUSR1": syscall.SIGUSR1, "SIGUSR2": syscall.SIGUSR2, "SIGWINCH": syscall.SIGWINCH}
