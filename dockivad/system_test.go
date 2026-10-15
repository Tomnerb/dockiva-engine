package main

import (
	"archive/tar"
	"bufio"
	"bytes"
	"compress/gzip"
	"encoding/json"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const fixture = "../examples/m19/shell-image"

// archive is a tar of name: content, in order.
func archive(t *testing.T, files ...[2]string) []byte {
	t.Helper()
	var buf bytes.Buffer
	w := tar.NewWriter(&buf)
	for _, f := range files {
		check(w.WriteHeader(&tar.Header{Name: f[0], Mode: 0o644, Size: int64(len(f[1])), Typeflag: tar.TypeReg}))
		_, e := io.WriteString(w, f[1])
		check(e)
	}
	check(w.Close())
	return buf.Bytes()
}

func gzipped(data []byte) []byte {
	var buf bytes.Buffer
	gz := gzip.NewWriter(&buf)
	_, e := gz.Write(data)
	check(e)
	check(gz.Close())
	return buf.Bytes()
}

// fixtureBlobs is the fixture image's config and (gzip) layer.
func fixtureBlobs(t *testing.T) (config, layer []byte) {
	t.Helper()
	manifest, _ := imageDocs(fixture)
	config = readBounded(fixture, "blobs/sha256/"+str(object(manifest["config"])["digest"])[7:], maxJSON)
	layer = readBounded(fixture, "blobs/sha256/"+str(object(array(manifest["layers"])[0])["digest"])[7:], maxBlob)
	return config, layer
}

// classicArchive is a pre-25 docker save tar: <id>.json, <id>/layer.tar, manifest.json.
func classicArchive(t *testing.T, config, layer []byte, tags string) []byte {
	return archive(t, [2]string{"abc.json", string(config)}, [2]string{"l1/layer.tar", string(layer)},
		[2]string{"manifest.json", `[{"Config":"abc.json","RepoTags":` + tags + `,"Layers":["l1/layer.tar"]}]`})
}

// load posts an archive to /images/load and returns its stream text and error.
func load(t *testing.T, b *engine, body []byte, query string) (stream, failure string) {
	t.Helper()
	r := request(t, b.socketHandler(), "POST", "/v1.45/images/load"+query, string(body), "")
	if r.Code != 200 || r.Header().Get("Content-Type") != "application/json" {
		t.Fatalf("load: %d %s", r.Code, r.Body.String())
	}
	for _, line := range strings.Split(strings.TrimSpace(r.Body.String()), "\n") {
		var m obj
		check(json.Unmarshal([]byte(line), &m))
		if s, ok := m["stream"].(string); ok {
			stream += s
		}
		if e, ok := m["error"].(string); ok {
			failure = e
		}
	}
	return stream, failure
}

func decodeBody(t *testing.T, body string) obj {
	t.Helper()
	m := obj{}
	check(json.Unmarshal([]byte(body), &m))
	return m
}

func TestSystemDiskUsage(t *testing.T) {
	b := testEngine(t)
	h := b.socketHandler()
	if r := request(t, h, "POST", "/containers/create?name=user", `{"Image":"dockiva/shell:demo","HostConfig":{"Binds":["data:/data"]}}`, ""); r.Code != 201 {
		t.Fatalf("create: %d %s", r.Code, r.Body.String())
	}
	check(os.WriteFile(filepath.Join(volumeData(b.Home, "data"), "f"), []byte("12345"), 0o600))
	check(os.Symlink("/etc", filepath.Join(volumeData(b.Home, "data"), "link"))) // not followed
	// One image with two tags is one row.
	if r := request(t, h, "POST", "/images/dockiva/shell:demo/tag?repo=test/other&tag=1", "", ""); r.Code != 201 {
		t.Fatalf("tag: %d %s", r.Code, r.Body.String())
	}
	r := request(t, h, "GET", "/v1.45/system/df", "", "")
	if r.Code != 200 {
		t.Fatalf("df: %d %s", r.Code, r.Body.String())
	}
	df := decodeBody(t, r.Body.String())
	images, containers, volumes := df["Images"].([]any), df["Containers"].([]any), df["Volumes"].([]any)
	if df["LayersSize"].(float64) <= 0 || len(images) != 1 || len(containers) != 1 || len(volumes) != 1 || len(df["BuildCache"].([]any)) != 0 {
		t.Fatalf("df: %s", r.Body.String())
	}
	im := images[0].(obj)
	if im["Containers"] != 1.0 || im["SharedSize"] != 0.0 || im["Size"] != df["LayersSize"] || len(im["RepoTags"].([]any)) != 2 {
		t.Fatalf("image row: %v", im)
	}
	c := containers[0].(obj)
	if c["Names"].([]any)[0] != "/user" || c["State"] != "created" || c["SizeRw"].(float64) <= 0 || c["SizeRootFs"] != c["SizeRw"] {
		t.Fatalf("container row: %v", c)
	}
	v := volumes[0].(obj)
	if usage := v["UsageData"].(obj); v["Name"] != "data" || usage["Size"] != 5.0 || usage["RefCount"] != 1.0 {
		t.Fatalf("volume row: %v", v)
	}
}

func TestLoadBothFormats(t *testing.T) {
	b := testEngine(t)
	want := loadImage(fixture).Digest

	// Docker 25+ format, from docker save: an OCI layout beside manifest.json.
	saved := request(t, b.socketHandler(), "GET", "/v1.45/images/get?names=dockiva/shell:demo", "", "")
	if saved.Code != 200 || saved.Header().Get("Content-Type") != "application/x-tar" {
		t.Fatalf("save: %d %s", saved.Code, saved.Body.String())
	}
	request(t, b.socketHandler(), "DELETE", "/images/dockiva/shell:demo", "", "")
	stream, failure := load(t, b, saved.Body.Bytes(), "?quiet=1")
	if failure != "" || stream != "Loaded image: dockiva/shell:demo\n" || b.image("dockiva/shell:demo").Digest != want {
		t.Fatalf("OCI load: %q %q", stream, failure)
	}

	// The classic format, gzip-compressed, with an uncompressed layer.
	config, layer := fixtureBlobs(t)
	gz, e := gzip.NewReader(bytes.NewReader(layer))
	check(e)
	plain, e := io.ReadAll(gz)
	check(e)
	stream, failure = load(t, b, gzipped(classicArchive(t, config, plain, `["test/classic:1","test/classic:2"]`)), "")
	if failure != "" || !strings.Contains(stream, "Loaded image: test/classic:1\nLoaded image: test/classic:2\n") {
		t.Fatalf("classic load: %q %q", stream, failure)
	}
	if im := b.image("test/classic:2"); im.Digest != b.image("test/classic:1").Digest || !exists(filepath.Join(im.Root, "bin")) {
		t.Fatal("classic tags differ or are not unpacked")
	}

	// Untagged, with the layer still gzip: named as builds name untagged images.
	stream, failure = load(t, b, classicArchive(t, config, layer, `null`), "")
	id := strings.TrimSpace(strings.TrimPrefix(stream, "Loaded image ID: "))
	if failure != "" || !digestPattern.MatchString(id) || b.image("<none>:"+id[7:19]).Digest != id {
		t.Fatalf("untagged load: %q %q", stream, failure)
	}

	// A multi-platform OCI index: linux/arm64 is picked.
	manifest := readBounded(fixture, "blobs/sha256/"+want[7:], maxJSON)
	nested, _ := json.Marshal(obj{"schemaVersion": 2, "mediaType": indexMedia, "manifests": []any{
		obj{"mediaType": manifestMedia, "digest": "sha256:" + strings.Repeat("0", 64), "size": 1, "platform": obj{"os": "linux", "architecture": "amd64"}},
		obj{"mediaType": manifestMedia, "digest": want, "size": len(manifest), "platform": obj{"os": "linux", "architecture": "arm64"}}}})
	index, _ := json.Marshal(obj{"schemaVersion": 2, "manifests": []any{obj{"mediaType": indexMedia, "digest": digest(nested), "size": len(nested), "annotations": obj{"io.containerd.image.name": "docker.io/test/multi:1"}}}})
	blobs := [][2]string{{"oci-layout", `{"imageLayoutVersion":"1.0.0"}`}, {"index.json", string(index)}, {"blobs/sha256/" + digest(nested)[7:], string(nested)}, {"blobs/sha256/" + want[7:], string(manifest)}}
	for _, d := range []string{digest(config), digest(layer)} {
		blobs = append(blobs, [2]string{"blobs/sha256/" + d[7:], string(readBounded(fixture, "blobs/sha256/"+d[7:], maxBlob))})
	}
	stream, failure = load(t, b, archive(t, blobs...), "")
	if failure != "" || stream != "Loaded image: test/multi:1\n" || b.image("test/multi:1").Digest != want {
		t.Fatalf("index load: %q %q", stream, failure)
	}
}

func TestLoadSkipsOtherPlatformsAndUnsafePaths(t *testing.T) {
	b := testEngine(t)
	config, layer := fixtureBlobs(t)
	amd64 := strings.Replace(string(config), `"architecture":"arm64"`, `"architecture":"amd64"`, 1)
	if amd64 == string(config) {
		t.Fatalf("fixture config: %s", config)
	}
	stream, failure := load(t, b, classicArchive(t, []byte(amd64), layer, `["test/x86:1"]`), "")
	if !strings.Contains(stream, "Skipped test/x86:1: it is linux/amd64") || !strings.Contains(failure, "No linux/arm64 image") || exists(filepath.Join(b.Cache, key("test/x86:1"))) {
		t.Fatalf("amd64 image: %q %q", stream, failure)
	}
	for name, body := range map[string][]byte{
		"traversal": archive(t, [2]string{"../escape", "x"}),
		"absolute":  archive(t, [2]string{"/tmp/escape", "x"}),
		"not a tar": []byte(strings.Repeat("x", 2048)),
	} {
		if _, failure := load(t, b, body, ""); failure == "" {
			t.Fatalf("%s accepted", name)
		}
	}
	// Nothing is written through a symlink in the archive.
	outside := t.TempDir()
	var buf bytes.Buffer
	w := tar.NewWriter(&buf)
	check(w.WriteHeader(&tar.Header{Name: "blobs", Typeflag: tar.TypeSymlink, Linkname: outside}))
	check(w.WriteHeader(&tar.Header{Name: "blobs/x", Mode: 0o644, Size: 1, Typeflag: tar.TypeReg}))
	_, e := w.Write([]byte("x"))
	check(e)
	check(w.Close())
	if _, failure := load(t, b, buf.Bytes(), ""); failure == "" || exists(filepath.Join(outside, "x")) {
		t.Fatalf("symlink escape: %q", failure)
	}
}

func TestPrune(t *testing.T) {
	b := testEngine(t)
	h := b.socketHandler()
	for name, label := range map[string]string{"keep": "no", "gone": "yes"} {
		if r := request(t, h, "POST", "/containers/create?name="+name, `{"Image":"dockiva/shell:demo","Labels":{"prune":"`+label+`"}}`, ""); r.Code != 201 {
			t.Fatalf("create: %d %s", r.Code, r.Body.String())
		}
	}
	if r := request(t, h, "POST", "/containers/prune?filters=%7B%22status%22:%5B%22exited%22%5D%7D", "", ""); r.Code != 400 {
		t.Fatalf("unsupported filter: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "POST", "/containers/prune?filters=%7B%22until%22:%5B%221h%22%5D%7D", "", ""); r.Code != 200 || len(decodeBody(t, r.Body.String())["ContainersDeleted"].([]any)) != 0 {
		t.Fatalf("until: %d %s", r.Code, r.Body.String())
	}
	r := request(t, h, "POST", "/containers/prune?filters=%7B%22label%22:%5B%22prune=yes%22%5D%7D", "", "")
	out := decodeBody(t, r.Body.String())
	if deleted := out["ContainersDeleted"].([]any); r.Code != 200 || len(deleted) != 1 || deleted[0] != key("gone") || out["SpaceReclaimed"].(float64) <= 0 {
		t.Fatalf("container prune: %d %s", r.Code, r.Body.String())
	}
	if exists(b.container("gone")) || !exists(b.container("keep")) {
		t.Fatal("wrong container pruned")
	}

	// Images: a dangling (untagged) image and a tagged one that no container uses.
	config, layer := fixtureBlobs(t)
	config = []byte(strings.Replace(string(config), `"architecture":"arm64"`, `"architecture":"arm64","author":"prune"`, 1)) // another image
	stream, _ := load(t, b, classicArchive(t, config, layer, `null`), "")
	dangling := strings.TrimSpace(strings.TrimPrefix(stream, "Loaded image ID: "))
	load(t, b, classicArchive(t, config, layer, `["test/unused:1"]`), "")
	r = request(t, h, "POST", "/images/prune?filters=%7B%22dangling%22:%5B%22true%22%5D%7D", "", "")
	out = decodeBody(t, r.Body.String())
	if deleted := out["ImagesDeleted"].([]any); r.Code != 200 || len(deleted) != 0 || out["SpaceReclaimed"] != 0.0 {
		// the untagged and the tagged loads are the same image: test/unused:1 keeps it
		t.Fatalf("dangling prune of a tagged image: %d %s", r.Code, r.Body.String())
	}
	request(t, h, "DELETE", "/images/test/unused:1", "", "")
	r = request(t, h, "POST", "/images/prune", "", "")
	out = decodeBody(t, r.Body.String())
	if deleted := out["ImagesDeleted"].([]any); r.Code != 200 || len(deleted) != 1 || deleted[0].(obj)["Deleted"] != dangling || out["SpaceReclaimed"].(float64) <= 0 {
		t.Fatalf("dangling prune: %d %s", r.Code, r.Body.String())
	}
	load(t, b, classicArchive(t, config, layer, `["test/unused:1"]`), "")
	r = request(t, h, "POST", "/images/prune?filters=%7B%22dangling%22:%5B%22false%22%5D%7D", "", "")
	out = decodeBody(t, r.Body.String())
	if deleted := out["ImagesDeleted"].([]any); r.Code != 200 || len(deleted) != 2 || deleted[0].(obj)["Untagged"] != "test/unused:1" || deleted[1].(obj)["Deleted"] != dangling {
		t.Fatalf("prune -a: %d %s", r.Code, r.Body.String())
	}
	if !exists(filepath.Join(b.Cache, key("dockiva/shell:demo"))) {
		t.Fatal("prune -a removed an image a container uses")
	}
	if r := request(t, h, "POST", "/build/prune?all=1", "", ""); r.Code != 200 || r.Body.String() != `{"CachesDeleted":[],"SpaceReclaimed":0}` {
		t.Fatalf("build prune: %d %s", r.Code, r.Body.String())
	}
}

// TestShutdownStopsContainers stands in a fake supervisor for a running container:
// it holds the container lock and answers the stop command as the real one does.
func TestShutdownStopsContainers(t *testing.T) {
	b := testEngine(t)
	b.quit = make(chan struct{}, 1)
	h := b.socketHandler()
	for _, name := range []string{"web", "idle"} {
		if r := request(t, h, "POST", "/containers/create?name="+name, `{"Image":"dockiva/shell:demo","HostConfig":{"RestartPolicy":{"Name":"unless-stopped"}}}`, ""); r.Code != 201 {
			t.Fatalf("create: %d %s", r.Code, r.Body.String())
		}
	}
	idle := b.container("idle")
	save(filepath.Join(idle, "status.json"), obj{"state": "exited", "exit_code": 0, "stopped_by_user": true}) // docker stop earlier
	p := b.container("web")
	lock, e := lockFile(filepath.Join(p, "lock"), true)
	check(e)
	listener, e := net.Listen("tcp", "127.0.0.1:0")
	check(e)
	defer listener.Close()
	save(filepath.Join(p, "control.json"), obj{"port": listener.Addr().(*net.TCPAddr).Port, "token": "secret"})
	save(filepath.Join(p, "status.json"), obj{"state": "running", "generation": "g", "pid": os.Getpid()})
	commands := make(chan string, 1)
	go func() {
		conn, e := listener.Accept()
		if e != nil {
			return
		}
		defer conn.Close()
		line, _ := bufio.NewReader(conn).ReadString('\n')
		commands <- line
		save(filepath.Join(p, "status.json"), obj{"state": "exited", "exit_code": 143, "generation": "g", "stopped_by_user": true})
		lock.Close()
		_, _ = conn.Write([]byte("OK"))
	}()
	if r := request(t, h, "POST", "/v1.45/dockiva/shutdown", "", ""); r.Code != 204 {
		t.Fatalf("shutdown: %d %s", r.Code, r.Body.String())
	}
	if line := <-commands; line != "secret stop\n" { // docker stop's signal and grace period
		t.Fatalf("supervisor command: %q", line)
	}
	if s := readObject(filepath.Join(p, "status.json")); s["state"] != "exited" || s["stopped_by_user"] != nil {
		t.Fatalf("stopped for shutdown: %v", s)
	}
	if readObject(filepath.Join(idle, "status.json"))["stopped_by_user"] != true {
		t.Fatal("a container the user stopped lost its mark")
	}
	select {
	case <-b.quit:
	default:
		t.Fatal("the engine was not asked to exit")
	}
}

// docker create with "Cmd": [] runs the Entrypoint alone; with neither, it fails.
func TestEmptyCmd(t *testing.T) {
	b := testEngine(t)
	h := b.socketHandler()
	if r := request(t, h, "POST", "/containers/create?name=entry", `{"Image":"dockiva/shell:demo","Cmd":[]}`, ""); r.Code != 201 {
		t.Fatalf("empty Cmd: %d %s", r.Code, r.Body.String())
	}
	if item := b.inspect("entry"); item["Path"] != "/bin/sh" || len(item["Args"].([]string)) != 0 {
		t.Fatalf("empty Cmd command: %v %v", item["Path"], item["Args"])
	}
	config, layer := fixtureBlobs(t)
	bare := strings.Replace(string(config), `"Entrypoint":["/bin/sh"],`, "", 1) // CMD only
	if _, failure := load(t, b, classicArchive(t, []byte(bare), layer, `["test/bare:1"]`), ""); failure != "" || bare == string(config) {
		t.Fatalf("load: %q", failure)
	}
	if r := request(t, h, "POST", "/containers/create?name=nothing", `{"Image":"test/bare:1","Cmd":[]}`, ""); r.Code != 400 || !strings.Contains(r.Body.String(), "No command specified") {
		t.Fatalf("no command: %d %s", r.Code, r.Body.String())
	}
}

// docker tag sends repo=docker.io/...; the tag outlives the image it was made from.
func TestTagThenRemoveSource(t *testing.T) {
	b := testEngine(t)
	h := b.socketHandler()
	if r := request(t, h, "POST", "/images/dockiva/shell:demo/tag?repo=docker.io/test/copy&tag=latest", "", ""); r.Code != 201 {
		t.Fatalf("tag: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "DELETE", "/images/dockiva/shell:demo", "", ""); r.Code != 200 || strings.Contains(r.Body.String(), "Deleted") {
		t.Fatalf("rmi: %d %s", r.Code, r.Body.String())
	}
	r := request(t, h, "GET", "/images/json", "", "")
	if r.Code != 200 || !strings.Contains(r.Body.String(), `"RepoTags":["test/copy:latest"]`) {
		t.Fatalf("images: %d %s", r.Code, r.Body.String())
	}
	for _, name := range []string{"test/copy", "docker.io/test/copy:latest"} {
		if r := request(t, h, "GET", "/images/"+name+"/json", "", ""); r.Code != 200 {
			t.Fatalf("inspect %s: %d %s", name, r.Code, r.Body.String())
		}
	}
	// A name an earlier engine cached unshortened moves to its familiar name.
	commitImage(b.Cache, "test/old:1", noLinks(b.imagePath("test/copy"), "layout"))
	check(os.Rename(filepath.Join(b.Cache, key("test/old:1")), filepath.Join(b.Cache, key("docker.io/test/old:1"))))
	meta := filepath.Join(b.Cache, key("docker.io/test/old:1"), "image.json")
	m := readObject(meta)
	m["name"] = "docker.io/test/old:1"
	save(meta, m)
	migrateNames(b.Cache)
	if r := request(t, h, "DELETE", "/images/test/old:1", "", ""); r.Code != 200 || exists(filepath.Join(b.Cache, key("docker.io/test/old:1"))) {
		t.Fatalf("rmi of a migrated name: %d %s", r.Code, r.Body.String())
	}
}
