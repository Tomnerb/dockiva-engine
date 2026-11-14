package main

import (
	"archive/tar"
	"bytes"
	"cmp"
	"compress/gzip"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"unicode/utf8"
	"unsafe"

	"github.com/klauspost/compress/zstd"
)

// OCI and Docker v2 media types (the same structures).
const manifestMedia = "application/vnd.oci.image.manifest.v1+json"
const configMedia = "application/vnd.oci.image.config.v1+json"
const layerMedia = "application/vnd.oci.image.layer.v1.tar"
const indexMedia = "application/vnd.oci.image.index.v1+json"
const dockerManifest = "application/vnd.docker.distribution.manifest.v2+json"
const dockerList = "application/vnd.docker.distribution.manifest.list.v2+json"
const dockerConfig = "application/vnd.docker.container.image.v1+json"

// layerTypes: the layer formats an image may use, and each one's compression.
var layerTypes = map[string]string{layerMedia: "", layerMedia + "+gzip": "gzip", layerMedia + "+zstd": "zstd",
	"application/vnd.docker.image.rootfs.diff.tar": "", "application/vnd.docker.image.rootfs.diff.tar.gzip": "gzip"}

// Images stream to disk and are unpacked once, so only sanity bounds remain.
const maxBlob = int64(64 << 30) // one layer, and a whole pull
const maxMembers = 4 << 20      // files in an image
const maxJSON = int64(1 << 20)

// execEntries is the runtime's cap on argv and environment entries
// (EXEC_ENTRIES in native/address_space.h); their bytes are capped at 1 MiB.
const execEntries = 16384

var digestPattern = regexp.MustCompile(`^sha256:[0-9a-f]{64}$`)

// image is an image's configuration and, in Root, its unpacked filesystem: the
// cache's (read only), or a container's own files (with Lower: over the cache's).
type image struct {
	Digest           string
	Entry, Args, Env []string
	WorkingDir       string   // absolute guest path; "/" when the image sets none
	StopSignal       string   // signal name sent on stop: SIGTERM unless the image sets one
	User             string   // USER: name or uid, with an optional :group or :gid; "" for root
	Volumes          []string // declared VOLUME paths, sorted
	Exposed          []string // EXPOSE: "80/tcp", "53/udp"
	Healthcheck      obj      // HEALTHCHECK, as Config.Healthcheck (Test, Interval, ...)
	Root             string
	Lower            string // beneath Root, a container's image files (layers); "" for none
	Arch             string // "arm64", or "amd64" (run through FEX: x86.go)
	layout           string
	descriptor       obj
	blobs            []string // manifest, config and layer digests
	layers           []obj
	diffs            []string
	mounts           []guestMount // a container's bind mounts and volumes, over Root and Lower (command)
}

func pathName(name string) string {
	reject(len(name) <= 4096 && !strings.HasPrefix(name, "/"), "Invalid image path")
	reject(printableUTF8(name), "Paths must be UTF-8 without control characters: "+strconv.Quote(name))
	out := []string{}
	for _, s := range strings.Split(name, "/") {
		reject(s != "..", "Path traversal rejected")
		reject(len(s) <= 255, "Path component too long")
		if s != "" && s != "." {
			out = append(out, s)
		}
	}
	reject(len(out) <= 256, "Path too deep")
	return strings.Join(out, "/")
}
func blob(root string, desc obj, limit int64, media ...string) []byte {
	d := blobDigest(desc, limit, media...)
	b := readBounded(root, "blobs/sha256/"+d[7:], limit)
	reject(int64(len(b)) == integer(desc["size"]) && digest(b) == d, "Blob size/digest mismatch")
	return b
}

// blobDigest validates a descriptor (local, of an allowed type, size within limit).
func blobDigest(desc obj, limit int64, media ...string) string {
	d := text(desc["digest"])
	reject(digestPattern.MatchString(d), "Invalid sha256 descriptor")
	ok := false
	for _, m := range media {
		ok = ok || desc["mediaType"] == m
	}
	reject(ok, "Unsupported descriptor media type: "+text(desc["mediaType"]))
	reject(idle(desc["urls"]), "External blobs (urls) unsupported")
	size := integer(desc["size"])
	reject(size >= 0 && size <= limit, "Invalid descriptor size")
	if data, embedded := desc["data"]; embedded { // OCI 1.1: the blob's bytes inline (Debian's configs); the same blob is fetched by digest
		raw, e := base64.StdEncoding.DecodeString(text(data))
		reject(e == nil && int64(len(raw)) == size && digest(raw) == d, "Embedded blob data does not match its descriptor")
	}
	return d
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

// printableUTF8: valid UTF-8 (what APFS stores) without control characters.
func printableUTF8(s string) bool {
	for _, c := range s {
		if c < 32 || c == 127 || c == utf8.RuneError {
			return false
		}
	}
	return true
}
func linkTarget(name string) string {
	reject(name != "" && len(name) <= 4096, "Invalid symlink target")
	reject(printableUTF8(name), "Symlink targets must be UTF-8 without control characters: "+strconv.Quote(name))
	return name
}

// loadImage reads an OCI layout's manifest and configuration; layers are read
// only when unpacking.
func loadImage(layout string) *image {
	header := readObject(filepath.Join(layout, "oci-layout"))
	reject(header["imageLayoutVersion"] == "1.0.0", "Unsupported OCI version")
	index := readObject(filepath.Join(layout, "index.json"))
	reject(integer(index["schemaVersion"]) == 2, "Invalid index version")
	items := array(index["manifests"])
	reject(len(items) == 1, "Expected exactly one image")
	desc := object(items[0])
	raw := blob(layout, desc, maxJSON, manifestMedia, dockerManifest)
	manifest := decode(raw, maxJSON)
	reject(integer(manifest["schemaVersion"]) == 2 && (manifest["mediaType"] == nil || manifest["mediaType"] == desc["mediaType"]), "Invalid image manifest")
	if p := desc["platform"]; p != nil {
		m := object(p)
		reject(m["os"] == "linux" && (m["architecture"] == "arm64" || m["architecture"] == "amd64"), "Only linux/arm64 and linux/amd64 supported")
	}
	im := &image{Digest: str(desc["digest"]), descriptor: desc, layout: layout, blobs: []string{str(desc["digest"])}}
	cd := object(manifest["config"])
	cr := blob(layout, cd, maxJSON, configMedia, dockerConfig)
	im.blobs = append(im.blobs, str(cd["digest"]))
	c := decode(cr, maxJSON)
	reject(c["os"] == "linux" && (c["architecture"] == "arm64" || c["architecture"] == "amd64"), "Only linux/arm64 and linux/amd64 supported")
	im.Arch = text(c["architecture"])
	settings := object(c["config"])
	im.User = validUser(text(settings["User"]))
	im.StopSignal = stopSignal(text(settings["StopSignal"]))
	im.Healthcheck = object(settings["Healthcheck"])
	im.Entry = stringsOf(settings["Entrypoint"])
	im.Args = stringsOf(settings["Cmd"])
	im.Env = stringsOf(settings["Env"])
	validateEnv(im.Env)
	layers := array(manifest["layers"])
	reject(len(layers) > 0 && len(layers) <= 128, "Expected 1..128 layers")
	rf := object(c["rootfs"])
	reject(rf["type"] == "layers", "Invalid rootfs")
	diffs := array(rf["diff_ids"])
	reject(len(layers) == len(diffs), "Layer/DiffID count mismatch")
	for i, v := range layers {
		ld := object(v)
		_, known := layerTypes[text(ld["mediaType"])]
		reject(known, "Unsupported layer format: "+text(ld["mediaType"]))
		im.blobs = append(im.blobs, blobDigest(ld, maxBlob, text(ld["mediaType"])))
		reject(digestPattern.MatchString(text(diffs[i])), "Invalid DiffID")
		im.layers, im.diffs = append(im.layers, ld), append(im.diffs, str(diffs[i]))
	}
	// Docker creates WorkingDir and declared volumes in each container (ensureDir).
	im.WorkingDir = "/"
	if wd := text(settings["WorkingDir"]); wd != "" {
		reject(strings.HasPrefix(wd, "/"), "WorkingDir must be absolute")
		pathName(wd[1:])
		im.WorkingDir = wd
	}
	for v := range object(settings["Volumes"]) {
		reject(strings.HasPrefix(v, "/"), "Volume must be absolute")
		pathName(v[1:])
		im.Volumes = append(im.Volumes, v)
	}
	sort.Strings(im.Volumes)
	for k := range object(settings["ExposedPorts"]) {
		if port, proto, _ := strings.Cut(k, "/"); proto == "" || proto == "tcp" || proto == "udp" { // not sctp
			im.Exposed = append(im.Exposed, port+"/"+cmp.Or(proto, "tcp"))
		}
	}
	return im
}

// unpack writes the image's layers, in order, into an empty directory, checking
// each layer's digest and DiffID as it streams. It returns the file bytes written.
func (im *image) unpack(root string) int64 {
	entries, e := os.ReadDir(root)
	check(e)
	reject(len(entries) == 0, "Unpacking requires an empty directory")
	var size int64
	members := 0
	for i, ld := range im.layers {
		d := str(ld["digest"])
		fd, e := syscall.Open(noLinks(im.layout, "blobs/sha256/"+d[7:]), syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
		check(e)
		f := os.NewFile(uintptr(fd), d)
		packed, counted := sha256.New(), &counter{}
		source := io.TeeReader(f, io.MultiWriter(packed, counted))
		var r io.Reader = source
		switch layerTypes[text(ld["mediaType"])] {
		case "gzip":
			gz, e := gzip.NewReader(source)
			check(errors.Join(e, nil))
			r = gz
		case "zstd":
			zr, e := zstd.NewReader(source, zstd.WithDecoderConcurrency(1))
			check(e)
			defer zr.Close()
			r = zr
		}
		diff := sha256.New()
		r = io.TeeReader(r, diff)
		size += applyLayer(root, r, &members)
		_, e = io.Copy(io.Discard, r) // tar padding and the end of the stream
		check(e)
		_, e = io.Copy(io.Discard, source)
		check(e)
		f.Close()
		reject(counted.n == integer(ld["size"]) && "sha256:"+hex.EncodeToString(packed.Sum(nil)) == d, "Layer digest mismatch")
		reject("sha256:"+hex.EncodeToString(diff.Sum(nil)) == im.diffs[i], "Layer DiffID mismatch")
	}
	// The runtime serves /dev/null, /dev/zero and /dev/urandom beneath /dev.
	if _, e := os.Lstat(filepath.Join(root, "dev")); os.IsNotExist(e) {
		check(mkdirGuest(filepath.Join(root, "dev")))
	}
	return size
}

type counter struct{ n int64 }

func (c *counter) Write(b []byte) (int, error) { c.n += int64(len(b)); return len(b), nil }

// applyLayer extracts one layer onto the unpacked root as Docker's overlay layers
// combine: ".wh.NAME" removes NAME from lower layers, ".wh..wh..opq" empties its
// directory of lower files, and no path goes through a symlink (layerDir). Devices
// and FIFOs are skipped: a container's /dev comes from the runtime.
//
// ponytail: APFS is case-insensitive by default, so of two names that differ only
// in case (terminfo has a few) the later one wins; a case-sensitive volume avoids it.
func applyLayer(root string, r io.Reader, members *int) int64 {
	t := tar.NewReader(r)
	created := map[string]bool{}
	var size int64
	for {
		h, e := t.Next()
		if e == io.EOF {
			break
		}
		check(e)
		*members++
		reject(*members <= maxMembers, "Too many files in the image")
		for k := range h.PAXRecords {
			reject(!strings.HasPrefix(k, "GNU.sparse"), "Sparse files unsupported")
		}
		name := pathName(h.Name)
		if name == "" {
			continue
		}
		parent, base := path.Split(name)
		parent = strings.TrimSuffix(parent, "/")
		if strings.HasPrefix(base, ".wh.") {
			dir, ok := layerDir(root, parent, false)
			if !ok {
				continue
			}
			if base == ".wh..wh..opq" {
				children, e := os.ReadDir(dir)
				check(e)
				for _, c := range children {
					if !created[path.Join(parent, c.Name())] {
						check(os.RemoveAll(filepath.Join(dir, c.Name())))
					}
				}
				continue
			}
			target := strings.TrimPrefix(base, ".wh.")
			reject(target != "" && target != "." && target != ".." && !strings.HasPrefix(target, ".wh."), "Invalid whiteout")
			if !created[path.Join(parent, target)] { // a whiteout hides lower layers only
				check(os.RemoveAll(filepath.Join(dir, target)))
			}
			continue
		}
		dir, _ := layerDir(root, parent, true)
		p := filepath.Join(dir, base)
		created[name] = true
		old, e := os.Lstat(p)
		if e == nil && !(old.IsDir() && h.Typeflag == tar.TypeDir) {
			check(os.RemoveAll(p))
		}
		mode := tarMode(h)
		switch h.Typeflag {
		case tar.TypeDir:
			if e != nil || !old.IsDir() {
				check(os.Mkdir(p, 0o700))
			}
			check(setMode(p, mode|0o700))
		case tar.TypeReg, tar.TypeRegA:
			f, e := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, mode&os.ModePerm|0o600)
			check(e)
			n, e := io.Copy(f, t)
			check(errors.Join(e, f.Close()))
			reject(n == h.Size, "Truncated layer file")
			size += n
			// After the write, which clears setuid and setgid; the engine's umask (077) also
			// took the group and other bits at create (an image's /etc/passwd is 0644).
			check(setMode(p, mode|0o600))
			_ = os.Chtimes(p, h.ModTime, h.ModTime)
		case tar.TypeSymlink:
			check(os.Symlink(linkTarget(h.Linkname), p))
		case tar.TypeLink:
			target := pathName(h.Linkname)
			reject(target != "" && target != name, "Invalid hardlink target")
			tparent, tbase := path.Split(target)
			tdir, ok := layerDir(root, strings.TrimSuffix(tparent, "/"), false)
			reject(ok, "Hardlink target missing: "+target)
			src := filepath.Join(tdir, tbase)
			st, e := os.Lstat(src)
			reject(e == nil && st.Mode().IsRegular(), "Hardlink target missing or not a regular file: "+target)
			check(os.Link(src, p))
		}
	}
	return size
}

// permBits are the mode bits unpacking and copying keep: rwx plus setuid, setgid
// and sticky (/tmp 1777, sudo 4755). The runtime reports them in the guest's stat.
const permBits = os.ModePerm | os.ModeSetuid | os.ModeSetgid | os.ModeSticky

// tarMode is a tar entry's permission bits, setuid, setgid and sticky included.
func tarMode(h *tar.Header) os.FileMode { return h.FileInfo().Mode() & permBits }

// setMode is os.Chmod for permBits. macOS refuses setgid (EPERM) on a file whose
// group the user is not in (a home under /tmp is group wheel), so such a file
// takes the user's own group first; the host group means nothing to the guest.
func setMode(p string, mode os.FileMode) error {
	if mode&os.ModeSetgid != 0 {
		if e := os.Lchown(p, -1, os.Getegid()); e != nil {
			return e
		}
	}
	return os.Chmod(p, mode)
}

// layerDir is a directory of an unpacked root, made when missing (create). A file
// or symlink on the way is refused (or, without create, reported absent), so
// extraction never writes through a guest link.
func layerDir(root, dir string, create bool) (string, bool) {
	p := root
	if dir == "" {
		return p, true
	}
	for _, part := range strings.Split(dir, "/") {
		p = filepath.Join(p, part)
		st, e := os.Lstat(p)
		if os.IsNotExist(e) {
			if !create {
				return "", false
			}
			func() {
				defer unlock(filepath.Dir(p), 0o700)()
				check(mkdirGuest(p))
			}()
			continue
		}
		check(e)
		if !st.IsDir() {
			reject(!create, "Not a directory (a file or symlink is in the way): /"+dir)
			return "", false
		}
	}
	return p, true
}

// cloneTree copies an unpacked root for a container: one APFS clone of the whole
// tree (clonefileat), sharing blocks until written; a plain copy elsewhere. A clone
// turns setuid and setgid off on files and directories (Debian's /var/mail is
// 2775), so they are set again. The copy has the modes of src, also where they keep
// the engine from reading it.
func cloneTree(src, dst string) {
	s, e := syscall.BytePtrFromString(src)
	check(e)
	d, e := syscall.BytePtrFromString(dst)
	check(e)
	cwd := -2 // AT_FDCWD
	const sysClonefileat, cloneNofollow = 462, 1
	clone := func() syscall.Errno {
		_, _, errno := syscall.Syscall6(sysClonefileat, uintptr(cwd), uintptr(unsafe.Pointer(s)), uintptr(cwd), uintptr(unsafe.Pointer(d)), cloneNofollow, 0)
		return errno
	}
	errno := clone()
	var held []locked
	defer func() { lockTree(dst, held); lockTree(src, held) }()
	if errno == syscall.EACCES { // a directory or file the guest left unreadable (restricted.go): nothing is copied
		unlockTree(src, &held)
		errno = clone()
	}
	if errno == 0 {
		// ponytail: one lstat per entry (~100 ms for 25k files); a list of an image's
		// setuid files, kept at unpack, would spare it.
		check(filepath.WalkDir(src, func(p string, entry fs.DirEntry, err error) error {
			check(err)
			if !entry.Type().IsRegular() && !entry.IsDir() {
				return nil
			}
			info, e := entry.Info()
			check(e)
			if info.Mode()&(os.ModeSetuid|os.ModeSetgid) != 0 {
				rel, e := filepath.Rel(src, p)
				check(e)
				check(setMode(filepath.Join(dst, rel), info.Mode()&permBits))
			}
			return nil
		}))
		return
	}
	out, e := exec.Command("/bin/cp", "-R", "-p", src, dst).CombinedOutput()
	reject(e == nil, "Copying the image failed: "+strings.TrimSpace(string(out)))
}

// guestFile is an executable file at an absolute guest path of the unpacked root or
// a container's mounts, following guest symlinks inside them, with its first bytes
// (for #! and ELF checks).
func (im *image) guestFile(guest string) ([]byte, bool) {
	host, ok := "", false
	if safeRun(func() {
		v := guestView{im.files(), im.mounts}
		rel := resolveGuest(pathName(strings.TrimPrefix(guest, "/")), v.link)
		host, ok = v.find(rel)
		ok = ok && rel != ""
	}) != nil || !ok {
		return nil, false
	}
	fd, e := syscall.Open(host, syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC|syscall.O_NONBLOCK, 0)
	if e != nil {
		return nil, false
	}
	f := os.NewFile(uintptr(fd), filepath.Base(host))
	defer f.Close()
	st, e := f.Stat()
	if e != nil || !st.Mode().IsRegular() || st.Mode()&0o111 == 0 {
		return nil, false
	}
	head := make([]byte, 256)
	n, _ := io.ReadFull(f, head)
	return head[:n], true
}

// ensureDir makes an absolute guest directory exist in the unpacked root,
// following guest symlinks like the runtime does, and returns the path as given.
func (im *image) ensureDir(guest string) string {
	reject(strings.HasPrefix(guest, "/"), "Directory must be an absolute guest path: "+guest)
	r := im.files()
	if p := resolveGuest(pathName(guest[1:]), r.link); p != "" {
		if host, ok := r.find(p); !ok || !isDir(host) {
			r.upperDir(p, true)
		}
	}
	return guest
}

// files are the image's files as a container sees them.
func (im *image) files() layers { return layers{upper: im.Root, lower: im.Lower} }
func validateEnv(env []string) {
	reject(len(env) <= execEntries, "Too many environment values")
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
	reject(len(cmd) > 0, "No command specified")
	reject(len(cmd) <= execEntries, "Too many arguments")
	find := func(guest string) (struct{ Data []byte }, bool) {
		head, ok := im.guestFile(guest)
		return struct{ Data []byte }{head}, ok
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
	if strings.Contains(cmd[0], "/") && !strings.HasPrefix(cmd[0], "/") { // ./run.sh: from WORKDIR
		cmd[0] = path.Join(cmp.Or(im.WorkingDir, "/"), cmd[0])
	}
	reject(strings.HasPrefix(cmd[0], "/"), "Executable not found in PATH: "+cmd[0])
	v, ok := find(cmd[0][1:])
	reject(ok, "Executable missing or not executable")
	// Like Linux execve: "#!INTERPRETER [ONE-ARGUMENT]" runs INTERPRETER [ARG] SCRIPT ARGS...;
	// an interpreter may itself be a script, four levels deep at most.
	for depth := 0; bytes.HasPrefix(v.Data, []byte("#!")); depth++ {
		reject(depth < 4, "Script interpreters nest too deeply: "+cmd[0])
		line := v.Data[2:]
		if i := bytes.IndexByte(line, '\n'); i >= 0 {
			line = line[:i]
		}
		reject(len(line) <= 255, "Script interpreter line too long")
		fields := strings.TrimSpace(strings.TrimSuffix(string(line), "\r"))
		interpreter, argument, _ := strings.Cut(fields, " ")
		interpreter, argument = strings.TrimSpace(interpreter), strings.TrimSpace(argument)
		reject(strings.HasPrefix(interpreter, "/"), "Script interpreter must be an absolute path")
		script := []string{interpreter, cmd[0]}
		if argument != "" {
			script = []string{interpreter, argument, cmd[0]}
		}
		cmd = append(script, cmd[1:]...)
		reject(len(cmd) <= execEntries, "Too many arguments")
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

// commitImage caches an OCI layout under name (its familiar form, which imagePath
// finds): its blobs (hard links where possible) and its unpacked filesystem,
// which containers clone.
func commitImage(cache, name, layout string) string {
	name = shortName(name) // docker tag sends repo=docker.io/...
	cache = private(cache)
	im := loadImage(layout)
	target := filepath.Join(cache, key(name))
	if exists(target) {
		old := loadImage(noLinks(target, "layout"))
		reject(old.Digest == im.Digest, "Cached name already has another digest")
		ensureUnpacked(target)
		return filepath.Join(target, "layout")
	}
	stage, e := os.MkdirTemp(cache, ".incoming-")
	check(e)
	defer removeTree(stage)
	dest := filepath.Join(stage, "layout")
	check(os.MkdirAll(filepath.Join(dest, "blobs/sha256"), 0700))
	for _, d := range im.blobs {
		if exists(filepath.Join(dest, "blobs/sha256", d[7:])) { // listed twice (the empty layer)
			continue
		}
		linkOrCopy(noLinks(layout, "blobs/sha256/"+d[7:]), filepath.Join(dest, "blobs/sha256", d[7:]))
	}
	save(filepath.Join(dest, "oci-layout"), obj{"imageLayoutVersion": "1.0.0"})
	save(filepath.Join(dest, "index.json"), obj{"schemaVersion": 2, "manifests": []any{im.descriptor}})
	im.layout = dest
	var size any
	if source := filepath.Dir(layout); filepath.Base(layout) == "layout" && exists(filepath.Join(source, "rootfs")) && exists(filepath.Join(source, "image.json")) {
		cloneTree(noLinks(source, "rootfs"), filepath.Join(stage, "rootfs")) // docker tag: the same files
		size = readObject(filepath.Join(source, "image.json"))["size"]
	} else {
		check(mkdirGuest(filepath.Join(stage, "rootfs")))
		size = im.unpack(filepath.Join(stage, "rootfs"))
	}
	save(filepath.Join(stage, "image.json"), obj{"name": name, "digest": im.Digest, "size": size})
	if e := os.Rename(stage, target); e != nil && !exists(target) { // a concurrent pull of the same name may win
		check(e)
	}
	return filepath.Join(target, "layout")
}

// commitTag is commitImage of an image with this digest; a name another image has
// moves to it (docker build -t, docker load).
func (b *engine) commitTag(name, digest, layout string) string {
	name = shortName(name)
	target := filepath.Join(b.Cache, key(name))
	if exists(target) && text(readObject(filepath.Join(target, "image.json"))["digest"]) != digest {
		retainLowers(b.storeDir(), b.State, target)
		gone := filepath.Join(b.Cache, ".replaced-"+token()[:16])
		check(os.Rename(target, gone))
		go removeTree(gone)
	}
	return commitImage(b.Cache, name, layout)
}

// commitPulled is commitImage for docker pull: a tag the registry has moved to
// another image (alpine:latest after an update) moves to it, as in Docker. The move
// holds the engine lock, so a container being created sees the old image or the new
// one; containers already on the old one keep its files (commitTag).
// ponytail: the lock covers unpacking the moved tag's new image; stage it unlocked
// first if pulls of updated tags stall other requests.
func (b *engine) commitPulled(name, layout string) string {
	if !exists(filepath.Join(b.Cache, key(shortName(name)))) {
		return commitImage(b.Cache, name, layout) // no lock: staged, then renamed in
	}
	b.Mu.Lock()
	defer b.Mu.Unlock()
	return b.commitTag(name, loadImage(layout).Digest, layout)
}

// migrateNames moves images cached under a docker.io/ name (docker tag did, before
// commitImage shortened names) to the familiar name imagePath looks up. One whose
// familiar name is taken was unreachable anyway, and goes. So does a cache directory
// without its image.json: docker rmi removed that first, then failed on the image's
// files where a directory lacked write permission, and listing images failed on it.
func migrateNames(cache string) {
	entries, e := os.ReadDir(cache)
	check(e)
	for _, entry := range entries {
		_ = safeRun(func() {
			dir := filepath.Join(cache, entry.Name())
			if len(entry.Name()) != 64 || !entry.IsDir() {
				return
			}
			if !exists(filepath.Join(dir, "image.json")) { // left by a removal that failed half way
				removeImageDir(filepath.Dir(cache), filepath.Join(filepath.Dir(cache), "containers"), dir)
				return
			}
			meta := readObject(filepath.Join(dir, "image.json"))
			name := str(meta["name"])
			if shortName(name) == name {
				return
			}
			target := filepath.Join(cache, key(shortName(name)))
			if exists(target) {
				removeImageDir(filepath.Dir(cache), filepath.Join(filepath.Dir(cache), "containers"), dir)
				return
			}
			meta["name"] = shortName(name)
			save(filepath.Join(dir, "image.json"), meta)
			check(os.Rename(dir, target))
		})
	}
}

// ensureUnpacked unpacks a cached image that predates unpacked images.
func ensureUnpacked(target string) {
	if exists(filepath.Join(target, "rootfs")) {
		return
	}
	im := loadImage(noLinks(target, "layout"))
	stage, e := os.MkdirTemp(target, ".rootfs-")
	check(e)
	defer removeTree(stage)
	size := im.unpack(stage)
	if e := os.Rename(stage, filepath.Join(target, "rootfs")); e != nil && !exists(filepath.Join(target, "rootfs")) {
		check(e)
	}
	meta := readObject(filepath.Join(target, "image.json"))
	meta["size"] = size
	save(filepath.Join(target, "image.json"), meta)
}

func linkOrCopy(src, dst string) {
	if os.Link(src, dst) == nil {
		return
	}
	in, e := os.Open(src)
	check(e)
	defer in.Close()
	out, e := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	check(e)
	_, e = io.Copy(out, in)
	check(errors.Join(e, out.Close()))
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
