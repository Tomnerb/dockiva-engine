package main

// docker load and docker save (POST /images/load, GET /images/get). An archive is
// Docker's classic format (manifest.json: [{Config, RepoTags, Layers}]) or an OCI
// layout (oci-layout, index.json, blobs/sha256), which Docker 25+ writes beside
// manifest.json. Each image becomes an OCI layout of its own and is cached under
// its tags like a pull or a build.

import (
	"archive/tar"
	"bufio"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"syscall"
)

// loadedImage is an image found in an archive: a layout loadImage accepts, and its tags.
type loadedImage struct {
	layout string
	tags   []string
}

// capped fails a read past n bytes: an archive is unpacked to disk, so the disk
// and this bound (a whole pull's) limit it.
type capped struct {
	r io.Reader
	n int64
}

func (c *capped) Read(p []byte) (int, error) {
	if c.n <= 0 {
		return 0, apiError{413, "The archive exceeds 64 GiB"}
	}
	n, e := c.r.Read(p[:min(int64(len(p)), c.n)])
	c.n -= int64(n)
	return n, e
}

// loadImages serves POST /images/load: the archive (plain or gzip) is unpacked to
// disk, then each linux/arm64 image in it is cached under its tags. Progress is
// JSON messages, as the docker CLI expects; quiet leaves out the layer lines.
func (b *engine) loadImages(w http.ResponseWriter, body io.Reader, quiet bool) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(200)
	enc := json.NewEncoder(w)
	say := func(m obj) {
		_ = enc.Encode(m)
		if f, ok := w.(http.Flusher); ok {
			f.Flush()
		}
	}
	defer func() {
		if v := recover(); v != nil {
			message := buildError(v)
			say(obj{"errorDetail": obj{"message": message}, "error": message})
		}
	}()
	work, e := os.MkdirTemp(private(filepath.Join(b.Home, "load")), "l-")
	check(e)
	defer func() { go os.RemoveAll(work) }()
	in := bufio.NewReaderSize(body, 1<<16)
	var r io.Reader = in
	if head, _ := in.Peek(2); len(head) == 2 && head[0] == 0x1f && head[1] == 0x8b { // docker save | gzip
		gz, e := gzip.NewReader(in)
		check(e)
		r = gz
	}
	dir := filepath.Join(work, "archive")
	check(os.Mkdir(dir, 0o700))
	extractContext(dir, &capped{r, maxBlob}) // no "..", absolute paths or writes through links
	images, skipped := archiveImages(dir, work)
	for _, s := range skipped {
		say(obj{"stream": s + "\n"})
	}
	reject(len(images) > 0, "No linux/arm64 image in the archive: Dockiva Engine runs linux/arm64 only")
	for _, l := range images {
		im := loadImage(l.layout)
		if !quiet {
			for _, ld := range im.layers {
				n := integer(ld["size"])
				say(obj{"status": "Loading layer", "id": str(ld["digest"])[7:19], "progressDetail": obj{"current": n, "total": n}})
			}
		}
		if len(l.tags) == 0 { // as untagged builds are named
			b.commitTag("<none>:"+im.Digest[7:19], im.Digest, l.layout)
			say(obj{"stream": "Loaded image ID: " + im.Digest + "\n"})
			continue
		}
		layout := l.layout
		for _, t := range l.tags {
			layout = b.commitTag(t, im.Digest, layout) // later tags clone the first one's files
			say(obj{"stream": "Loaded image: " + t + "\n"})
		}
	}
}

// archiveImages finds an unpacked archive's images: from its OCI index when it has
// one (keeping the manifests, so image IDs survive the round trip), else from
// manifest.json. Images for another platform are skipped, with the reason.
func archiveImages(dir, work string) ([]loadedImage, []string) {
	if exists(filepath.Join(dir, "oci-layout")) && exists(filepath.Join(dir, "index.json")) {
		return ociImages(dir, work)
	}
	reject(exists(filepath.Join(dir, "manifest.json")), "Not a docker save archive: no manifest.json or index.json")
	return classicImages(dir, work)
}

// manifestEntries reads manifest.json, a JSON array (decode reads objects only).
func manifestEntries(dir string) []any {
	raw := readBounded(dir, "manifest.json", maxJSON)
	return array(decode(append(append([]byte(`{"m":`), raw...), '}'), maxJSON+8)["m"])
}

func ociImages(dir, work string) ([]loadedImage, []string) {
	reject(readObject(filepath.Join(dir, "oci-layout"))["imageLayoutVersion"] == "1.0.0", "Unsupported OCI layout version")
	tags := map[string][]string{} // config digest: manifest.json's RepoTags
	if exists(filepath.Join(dir, "manifest.json")) {
		for _, v := range manifestEntries(dir) {
			e := object(v)
			config := pathName(str(e["Config"]))
			if hex, ok := strings.CutPrefix(config, "blobs/sha256/"); ok {
				tags["sha256:"+hex] = append(tags["sha256:"+hex], stringsOf(e["RepoTags"])...)
			}
		}
	}
	out, skipped, seen := []loadedImage{}, []string{}, map[string]int{}
	for _, v := range array(readObject(filepath.Join(dir, "index.json"))["manifests"]) {
		desc := object(v)
		annotations := object(desc["annotations"])
		name := text(annotations["io.containerd.image.name"])
		if ref := text(annotations["org.opencontainers.image.ref.name"]); name == "" && strings.ContainsAny(ref, ":/") {
			name = ref // a full reference; a bare tag names nothing
		}
		label := name
		if label == "" {
			label = text(desc["digest"])
		}
		if desc["mediaType"] == indexMedia || desc["mediaType"] == dockerList { // a multi-platform image
			var chosen obj
			platforms := []string{}
			for _, m := range array(decode(blob(dir, desc, maxJSON, indexMedia, dockerList), maxJSON)["manifests"]) {
				md := object(m)
				p := object(md["platform"])
				if p["os"] == "linux" && p["architecture"] == "arm64" && (idle(p["variant"]) || p["variant"] == "v8") {
					chosen = md
					break
				}
				platforms = append(platforms, text(p["os"])+"/"+text(p["architecture"]))
			}
			if chosen == nil {
				skipped = append(skipped, "Skipped "+label+": no linux/arm64 variant ("+strings.Join(platforms, ", ")+"); Dockiva Engine runs linux/arm64 only")
				continue
			}
			desc = chosen
		}
		manifest := decode(blob(dir, desc, maxJSON, manifestMedia, dockerManifest), maxJSON)
		cd := object(manifest["config"])
		if p := platformOf(decode(blob(dir, cd, maxJSON, configMedia, dockerConfig), maxJSON)); p != "linux/arm64" {
			skipped = append(skipped, "Skipped "+label+": it is "+p+"; Dockiva Engine runs linux/arm64 only")
			continue
		}
		d := str(desc["digest"])
		i, ok := seen[d]
		if !ok { // Docker lists an image once per tag
			digests := []string{d, str(cd["digest"])}
			for _, l := range array(manifest["layers"]) {
				digests = append(digests, str(object(l)["digest"]))
			}
			layout, blobs := newLayout(work)
			for _, d := range digests {
				reject(digestPattern.MatchString(d), "Invalid sha256 descriptor")
				if !exists(filepath.Join(blobs, d[7:])) {
					linkOrCopy(noLinks(dir, "blobs/sha256/"+d[7:]), filepath.Join(blobs, d[7:]))
				}
			}
			save(filepath.Join(layout, "index.json"), obj{"schemaVersion": 2, "manifests": []any{obj{"mediaType": desc["mediaType"], "digest": d, "size": desc["size"], "platform": obj{"os": "linux", "architecture": "arm64"}}}})
			i = len(out)
			seen[d] = i
			out = append(out, loadedImage{layout: layout})
			for _, t := range tags[str(cd["digest"])] {
				out[i].tags = addTag(out[i].tags, t)
			}
		}
		if name != "" {
			out[i].tags = addTag(out[i].tags, name)
		}
	}
	return out, skipped
}

// classicImages reads manifest.json: each image's config and layer files (tars,
// plain or compressed), written as an OCI layout with a manifest made for them.
func classicImages(dir, work string) ([]loadedImage, []string) {
	out, skipped := []loadedImage{}, []string{}
	for _, v := range manifestEntries(dir) {
		e := object(v)
		raw := readBounded(dir, pathName(str(e["Config"])), maxJSON)
		tags := []string{}
		for _, t := range stringsOf(e["RepoTags"]) {
			tags = addTag(tags, t)
		}
		if p := platformOf(decode(raw, maxJSON)); p != "linux/arm64" {
			label := digest(raw)
			if len(tags) > 0 {
				label = tags[0]
			}
			skipped = append(skipped, "Skipped "+label+": it is "+p+"; Dockiva Engine runs linux/arm64 only")
			continue
		}
		files := stringsOfAny(e["Layers"])
		reject(len(files) > 0 && len(files) <= 128, "Expected 1..128 layers")
		layout, blobs := newLayout(work)
		config := digest(raw)
		check(os.WriteFile(filepath.Join(blobs, config[7:]), raw, 0o600))
		layers := []any{}
		for _, f := range files {
			src := noLinks(dir, pathName(f))
			d, size, media := layerFile(src)
			if !exists(filepath.Join(blobs, d[7:])) {
				linkOrCopy(src, filepath.Join(blobs, d[7:]))
			}
			layers = append(layers, obj{"mediaType": media, "digest": d, "size": size})
		}
		id, n := writeJSONBlob(blobs, obj{"schemaVersion": 2, "mediaType": manifestMedia, "config": obj{"mediaType": configMedia, "digest": config, "size": len(raw)}, "layers": layers})
		save(filepath.Join(layout, "index.json"), obj{"schemaVersion": 2, "manifests": []any{obj{"mediaType": manifestMedia, "digest": id, "size": n, "platform": obj{"os": "linux", "architecture": "arm64"}}}})
		out = append(out, loadedImage{layout: layout, tags: tags})
	}
	return out, skipped
}

// newLayout makes an empty OCI layout in work; the caller adds blobs and index.json.
func newLayout(work string) (layout, blobs string) {
	layout, e := os.MkdirTemp(work, "image-")
	check(e)
	blobs = filepath.Join(layout, "blobs", "sha256")
	check(os.MkdirAll(blobs, 0o700))
	save(filepath.Join(layout, "oci-layout"), obj{"imageLayoutVersion": "1.0.0"})
	return layout, blobs
}

// layerFile is a layer file's digest, size and media type (by its compression).
func layerFile(p string) (string, int64, string) {
	fd, e := syscall.Open(p, syscall.O_RDONLY|syscall.O_NOFOLLOW, 0)
	check(e)
	f := os.NewFile(uintptr(fd), p)
	defer f.Close()
	st, e := f.Stat()
	check(e)
	reject(st.Mode().IsRegular(), "A layer is not a regular file: "+filepath.Base(p))
	head := make([]byte, 4)
	n, _ := io.ReadFull(f, head)
	_, e = f.Seek(0, io.SeekStart)
	check(e)
	h := sha256.New()
	size, e := io.Copy(h, f)
	check(e)
	media := layerMedia
	switch {
	case n >= 2 && head[0] == 0x1f && head[1] == 0x8b:
		media += "+gzip"
	case n == 4 && string(head) == "\x28\xb5\x2f\xfd":
		media += "+zstd"
	}
	return "sha256:" + hex.EncodeToString(h.Sum(nil)), size, media
}

// platformOf is an image config's os/architecture.
func platformOf(config obj) string { return text(config["os"]) + "/" + text(config["architecture"]) }

// addTag adds a tag, in the form images are cached under, once.
func addTag(tags []string, t string) []string {
	if t = normalizeTag(shortName(t)); !slices.Contains(tags, t) {
		tags = append(tags, t)
	}
	return tags
}

// saveImages serves docker save (GET /images/get?names=..., GET /images/NAME/get):
// a tar in Docker 25's format, an OCI layout with manifest.json beside it, which
// docker load of any version reads.
func (b *engine) saveImages(w http.ResponseWriter, names []string) {
	reject(len(names) > 0 && len(names) <= 64, "Save 1 to 64 images at once")
	type saved struct {
		name string
		im   *image
	}
	images := []saved{}
	for _, n := range names { // every name resolves before the response starts
		p := b.imagePath(n)
		images = append(images, saved{str(readObject(filepath.Join(p, "image.json"))["name"]), loadImage(noLinks(p, "layout"))})
	}
	w.Header().Set("Content-Type", "application/x-tar")
	w.WriteHeader(200)
	t := tar.NewWriter(w)
	file := func(name string, data []byte) {
		check(t.WriteHeader(&tar.Header{Name: name, Mode: 0o644, Size: int64(len(data)), Typeflag: tar.TypeReg}))
		_, e := t.Write(data)
		check(e)
	}
	for _, d := range []string{"blobs/", "blobs/sha256/"} {
		check(t.WriteHeader(&tar.Header{Name: d, Mode: 0o755, Typeflag: tar.TypeDir}))
	}
	written, index, manifests := map[string]bool{}, []any{}, []any{}
	for _, s := range images {
		for _, d := range s.im.blobs {
			if written[d] {
				continue
			}
			written[d] = true
			fd, e := syscall.Open(noLinks(s.im.layout, "blobs/sha256/"+d[7:]), syscall.O_RDONLY|syscall.O_NOFOLLOW, 0)
			check(e)
			f := os.NewFile(uintptr(fd), d)
			st, e := f.Stat()
			check(e)
			check(t.WriteHeader(&tar.Header{Name: "blobs/sha256/" + d[7:], Mode: 0o644, Size: st.Size(), Typeflag: tar.TypeReg}))
			_, e = io.Copy(t, f)
			f.Close()
			check(e)
		}
		desc := obj{}
		for k, v := range s.im.descriptor {
			desc[k] = v
		}
		tags := []string{}
		if !strings.HasPrefix(s.name, "<none>:") {
			tags = []string{s.name}
			desc["annotations"] = obj{"io.containerd.image.name": s.name, "org.opencontainers.image.ref.name": s.name[strings.LastIndex(s.name, ":")+1:]}
		}
		layers := []string{}
		for _, d := range s.im.blobs[2:] {
			layers = append(layers, "blobs/sha256/"+d[7:])
		}
		index = append(index, desc)
		manifests = append(manifests, obj{"Config": "blobs/sha256/" + s.im.blobs[1][7:], "RepoTags": tags, "Layers": layers})
	}
	for name, v := range map[string]any{"index.json": obj{"schemaVersion": 2, "mediaType": indexMedia, "manifests": index}, "manifest.json": manifests, "oci-layout": obj{"imageLayoutVersion": "1.0.0"}} {
		raw, e := json.Marshal(v)
		check(e)
		file(name, raw)
	}
	check(t.Close())
}
