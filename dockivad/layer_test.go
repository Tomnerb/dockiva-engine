package main

import (
	"archive/tar"
	"bytes"
	"encoding/json"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"syscall"
	"testing"
	"time"
)

// treeEntry is a file (data is its content), a directory (fs.ModeDir) or a symlink
// (fs.ModeSymlink; data is its target).
type treeEntry struct {
	name string
	mode fs.FileMode
	data string
}

var layerSample = []treeEntry{
	{"bin", fs.ModeDir | 0o755, ""},
	{"bin/link", fs.ModeSymlink, "tool"},
	{"bin/tool", 0o755, "#!/bin/sh"},
	{"data", fs.ModeDir | 0o755, ""},
	{"data/old", 0o644, "old"},
	{"dir2file", fs.ModeDir | 0o750, ""},
	{"dir2file/child", 0o644, "child"},
	{"etc", fs.ModeDir | 0o755, ""},
	{"etc/conf", 0o644, "a=1"},
	{"etc/keep", 0o600, "keep"},
	{"lib/deep", fs.ModeDir | 0o755, ""},
	{"lib/deep/so", 0o755, "ELF"},
	{"linkchange", fs.ModeSymlink, "a"},
	{"mode", 0o644, "mode"},
	{"swap", 0o644, "file"},
	{"via", fs.ModeSymlink, "etc"},
}

// buildTree makes the entries under root, files with a nanosecond mtime.
func buildTree(root string, entries []treeEntry) {
	when := time.Unix(1_700_000_000, 123_456_789)
	for _, x := range entries {
		p := filepath.Join(root, x.name)
		switch {
		case x.mode&fs.ModeSymlink != 0:
			check(os.Symlink(x.data, p))
		case x.mode.IsDir():
			check(os.MkdirAll(p, 0o755))
			check(os.Chmod(p, x.mode.Perm()))
		default:
			check(os.WriteFile(p, []byte(x.data), 0o600))
			check(os.Chmod(p, x.mode.Perm()))
			check(os.Chtimes(p, when, when))
		}
	}
}

// treeOf describes every entry under root: type and mode, file contents and their
// mtime in whole seconds (what a layer keeps), symlink targets.
func treeOf(root string) map[string]string {
	out := map[string]string{}
	check(filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		check(err)
		rel, e := filepath.Rel(root, p)
		check(e)
		info, e := d.Info()
		check(e)
		s := info.Mode().String()
		switch info.Mode().Type() {
		case 0:
			b, e := os.ReadFile(p)
			check(e)
			s += " " + string(b) + " @" + strconv.FormatInt(info.ModTime().Unix(), 10)
		case fs.ModeSymlink:
			s += " -> " + readTarget(p)
		}
		if rel != "." {
			out[rel] = s
		}
		return nil
	}))
	return out
}

func readTarget(p string) string { s, e := os.Readlink(p); check(e); return s }

// layerHeaders reads a written layer blob, checking its digest and size.
func layerHeaders(t *testing.T, blobs, d string, size int64) ([]byte, []*tar.Header) {
	t.Helper()
	raw, e := os.ReadFile(filepath.Join(blobs, d[7:]))
	check(e)
	if digest(raw) != d || int64(len(raw)) != size {
		t.Fatalf("blob %s: %d bytes, digest %s, reported size %d", d, len(raw), digest(raw), size)
	}
	headers := []*tar.Header{}
	for r := tar.NewReader(bytes.NewReader(raw)); ; {
		h, e := r.Next()
		if e == io.EOF {
			return raw, headers
		}
		check(e)
		if h.Uid != 0 || h.Gid != 0 || h.Uname != "" || h.Gname != "" || !h.AccessTime.IsZero() || !h.ChangeTime.IsZero() {
			t.Fatalf("owner or times leaked: %+v", h)
		}
		headers = append(headers, h)
	}
}

func headerNames(headers []*tar.Header) []string {
	names := []string{}
	for _, h := range headers {
		names = append(names, h.Name)
	}
	return names
}

func applyBlob(dst string, raw []byte) { n := 0; applyLayer(dst, bytes.NewReader(raw), &n) }

// layerDirs makes base (holding layerSample), root (base's clone) and blobs.
func layerDirs(t *testing.T) (dir, base, root, blobs string) {
	dir = t.TempDir()
	base, root, blobs = filepath.Join(dir, "base"), filepath.Join(dir, "root"), filepath.Join(dir, "blobs")
	check(os.Mkdir(base, 0o755))
	check(os.Mkdir(blobs, 0o700))
	buildTree(base, layerSample)
	cloneTree(base, root)
	return
}

func TestLayerRoundTrip(t *testing.T) {
	dir, base, root, blobs := layerDirs(t)
	at := func(name string) string { return filepath.Join(root, name) }
	check(os.WriteFile(at("etc/conf"), []byte("a=2"), 0o644)) // same size, new mtime
	check(os.WriteFile(at("etc/new.conf"), []byte("new"), 0o644))
	check(os.MkdirAll(at("newdir/sub"), 0o755))
	check(os.WriteFile(at("newdir/sub/file"), []byte("deep"), 0o644))
	check(os.Remove(at("data/old")))
	check(os.Chmod(at("data"), 0o700))
	check(os.RemoveAll(at("lib")))
	check(os.Chmod(at("mode"), 0o600))
	check(os.Remove(at("linkchange")))
	check(os.Symlink("b", at("linkchange")))
	check(os.Remove(at("swap"))) // a file becomes a directory
	check(os.Mkdir(at("swap"), 0o755))
	check(os.WriteFile(at("swap/inner"), []byte("in"), 0o644))
	check(os.RemoveAll(at("dir2file"))) // and a directory a file
	check(os.WriteFile(at("dir2file"), []byte("now a file"), 0o644))
	// A directory replacing a symlink: its files are new even where base's path, through
	// the old link, reaches an identical file (etc/keep, hard linked here).
	check(os.Remove(at("via")))
	check(os.Mkdir(at("via"), 0o755))
	check(os.Link(at("etc/keep"), at("via/keep")))

	d, size, changed := writeDiffLayer(root, base, blobs)
	raw, headers := layerHeaders(t, blobs, d, size)
	want := []string{"data/.wh.old", ".wh.lib", "data/", "dir2file", "etc/conf", "etc/new.conf", "linkchange", "mode",
		"newdir/", "newdir/sub/", "newdir/sub/file", "swap/", "swap/inner", "via/", "via/keep"}
	if names := headerNames(headers); !changed || !reflect.DeepEqual(names, want) {
		t.Fatalf("changed %v, layer %v, want %v", changed, names, want)
	}
	applied := filepath.Join(dir, "applied")
	cloneTree(base, applied)
	applyBlob(applied, raw)
	if got, want := treeOf(applied), treeOf(root); !reflect.DeepEqual(got, want) {
		t.Fatalf("applied layer differs from root:\n got %v\nwant %v", got, want)
	}
	// Two writes of the same trees: one blob, the same digest, no temporary file left.
	if again, _, _ := writeDiffLayer(root, base, blobs); again != d {
		t.Fatalf("not deterministic: %s then %s", d, again)
	}
	if left, _ := os.ReadDir(blobs); len(left) != 1 || left[0].Name() != d[7:] {
		t.Fatalf("blobs: %v", left)
	}
}

func TestLayerUnchanged(t *testing.T) {
	dir, base, root, blobs := layerDirs(t)
	d, size, changed := writeDiffLayer(root, base, blobs)
	raw, headers := layerHeaders(t, blobs, d, size)
	if changed || len(headers) != 0 || d != digest(make([]byte, 1024)) {
		t.Fatalf("unchanged tree: changed %v, %d entries, %s", changed, len(headers), d)
	}
	applied := filepath.Join(dir, "applied")
	cloneTree(base, applied)
	applyBlob(applied, raw)
	if !reflect.DeepEqual(treeOf(applied), treeOf(base)) {
		t.Fatal("an empty layer changed the tree")
	}
	// setuid alone is a change, and the layer keeps it.
	check(os.Chmod(filepath.Join(root, "bin/tool"), 0o755|fs.ModeSetuid))
	d, size, changed = writeDiffLayer(root, base, blobs)
	if _, headers = layerHeaders(t, blobs, d, size); !changed || len(headers) != 1 || headers[0].Name != "bin/tool" || headers[0].Mode != 0o4755 {
		t.Fatalf("setuid change: changed %v, %v", changed, headers)
	}
}

func TestLayerScratchImageDocs(t *testing.T) {
	dir := t.TempDir()
	root, layout := filepath.Join(dir, "root"), filepath.Join(dir, "layout")
	blobs := filepath.Join(layout, "blobs", "sha256")
	check(os.Mkdir(root, 0o755))
	check(os.MkdirAll(blobs, 0o700))
	buildTree(root, layerSample)
	d, size, changed := writeDiffLayer(root, "", blobs)
	if _, headers := layerHeaders(t, blobs, d, size); !changed || len(headers) != len(treeOf(root)) {
		t.Fatalf("scratch layer: %v", headerNames(headers))
	}
	put := func(media string, v any) obj {
		raw, e := json.Marshal(v)
		check(e)
		check(os.WriteFile(filepath.Join(blobs, digest(raw)[7:]), raw, 0o600))
		return obj{"mediaType": media, "digest": digest(raw), "size": len(raw)}
	}
	config := put(configMedia, obj{"os": "linux", "architecture": "arm64", "config": obj{"Cmd": []string{"/bin/tool"}},
		"rootfs": obj{"type": "layers", "diff_ids": []string{d}}})
	manifest := put(manifestMedia, obj{"schemaVersion": 2, "mediaType": manifestMedia, "config": config,
		"layers": []obj{{"mediaType": layerMedia, "digest": d, "size": size}}})
	save(filepath.Join(layout, "oci-layout"), obj{"imageLayoutVersion": "1.0.0"})
	save(filepath.Join(layout, "index.json"), obj{"schemaVersion": 2, "manifests": []obj{manifest}})

	m, c := imageDocs(layout)
	if text(object(array(m["layers"])[0])["digest"]) != d || text(array(object(c["rootfs"])["diff_ids"])[0]) != d ||
		!reflect.DeepEqual(stringsOf(object(c["config"])["Cmd"]), []string{"/bin/tool"}) {
		t.Fatalf("imageDocs: %v %v", m, c)
	}
	// The layer unpacks as an image (its digest, size and DiffID checked) to root's files.
	unpacked := filepath.Join(dir, "unpacked")
	check(os.Mkdir(unpacked, 0o755))
	loadImage(layout).unpack(unpacked)
	check(os.Remove(filepath.Join(unpacked, "dev"))) // unpack adds it
	if got, want := treeOf(unpacked), treeOf(root); !reflect.DeepEqual(got, want) {
		t.Fatalf("unpacked scratch layer differs:\n got %v\nwant %v", got, want)
	}
	mustReject(t, func() { imageDocs(dir) }) // no index.json
}

func TestLayerTreeSize(t *testing.T) {
	root := t.TempDir()
	buildTree(root, []treeEntry{{"a", 0o644, "12345"}, {"d", fs.ModeDir | 0o755, ""}, {"d/b", 0o644, "678"}, {"l", fs.ModeSymlink, "a"}})
	if n := treeSize(root); n != 8 {
		t.Fatalf("treeSize %d, want 8", n)
	}
}

// Unpacking keeps setuid, setgid and sticky (/tmp 1777, sudo 4755), and a layer
// written from the result records them again.
func TestSpecialModes(t *testing.T) {
	root, blobs := t.TempDir(), t.TempDir()
	headers := []tar.Header{
		{Name: "tmp/", Typeflag: tar.TypeDir, Mode: 0o1777},
		{Name: "usr/bin/sudo", Size: 3, Mode: 0o4755},
		{Name: "usr/bin/wall", Size: 3, Mode: 0o2755},
		{Name: "tmp/", Typeflag: tar.TypeDir, Mode: 0o1777}, // an existing directory again
	}
	applyBlob(root, layer(t, headers, []string{"", "ELF", "ELF"}))
	for name, want := range map[string]fs.FileMode{"tmp": fs.ModeDir | fs.ModeSticky | 0o777, "usr/bin/sudo": fs.ModeSetuid | 0o755, "usr/bin/wall": fs.ModeSetgid | 0o755} {
		if st, e := os.Lstat(filepath.Join(root, name)); e != nil || st.Mode() != want {
			t.Fatalf("%s: %v, want %v (%v)", name, st.Mode(), want, e)
		}
	}
	d, size, _ := writeDiffLayer(root, "", blobs)
	_, written := layerHeaders(t, blobs, d, size)
	modes := map[string]int64{}
	for _, h := range written {
		modes[h.Name] = h.Mode
	}
	if modes["tmp/"] != 0o1777 || modes["usr/bin/sudo"] != 0o4755 || modes["usr/bin/wall"] != 0o2755 {
		t.Fatalf("layer modes: %v", modes)
	}
}

// An APFS clone turns setuid and setgid off, on files and directories; cloneTree
// sets them again.
func TestCloneKeepsSetuid(t *testing.T) {
	src, dst := filepath.Join(t.TempDir(), "src"), filepath.Join(t.TempDir(), "dst")
	check(os.MkdirAll(filepath.Join(src, "bin"), 0o755))
	check(os.MkdirAll(filepath.Join(src, "var/mail"), 0o755))
	check(os.WriteFile(filepath.Join(src, "bin/sudo"), []byte("ELF"), 0o755))
	check(setMode(filepath.Join(src, "bin/sudo"), 0o755|fs.ModeSetuid|fs.ModeSetgid))
	check(setMode(filepath.Join(src, "var/mail"), 0o775|fs.ModeSetgid))
	cloneTree(src, dst)
	for name, want := range map[string]fs.FileMode{"bin/sudo": 0o755 | fs.ModeSetuid | fs.ModeSetgid, "var/mail": fs.ModeDir | 0o775 | fs.ModeSetgid} {
		st, e := os.Lstat(filepath.Join(dst, name))
		check(e)
		if st.Mode() != want {
			t.Fatalf("clone of %s: %v, want %v", name, st.Mode(), want)
		}
	}
}

// Under the engine's umask (077) an image's ordinary files keep their mode.
func TestUnpackModesUnderEngineUmask(t *testing.T) {
	old := syscall.Umask(0o077)
	defer syscall.Umask(old)
	root := t.TempDir()
	applyBlob(root, layer(t, []tar.Header{{Name: "etc/passwd", Size: 5, Mode: 0o644}, {Name: "bin/sh", Size: 3, Mode: 0o755}}, []string{"root:", "ELF"}))
	for name, want := range map[string]fs.FileMode{"etc/passwd": 0o644, "bin/sh": 0o755} {
		if st, e := os.Lstat(filepath.Join(root, name)); e != nil || st.Mode() != want {
			t.Fatalf("%s: %v, want %v (%v)", name, st.Mode(), want, e)
		}
	}
}
