package main

import (
	"archive/tar"
	"bytes"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func mustReject(t *testing.T, f func()) {
	t.Helper()
	if safeRun(f) == nil {
		t.Fatal("unsafe/invalid input accepted")
	}
}
func layer(t *testing.T, headers []tar.Header, contents []string) []byte {
	t.Helper()
	var out bytes.Buffer
	w := tar.NewWriter(&out)
	for i := range headers {
		check(w.WriteHeader(&headers[i]))
		if len(contents) > i {
			_, e := io.WriteString(w, contents[i])
			check(e)
		}
	}
	check(w.Close())
	return out.Bytes()
}
func TestLayerRules(t *testing.T) {
	for _, name := range []string{"../escape", "/escape", "a/../../escape", "a/\x01b", "a/\xffb"} {
		t.Run(name, func(t *testing.T) { mustReject(t, func() { pathName(name) }) })
	}
	pathName("units/system-systemd\\x2dcryptsetup.slice") // Linux names may hold \ and :
	pathName("ca/Főtanúsítvány.pem")
	apply := func(root string, headers []tar.Header, contents []string) {
		n := 0
		applyLayer(root, bytes.NewReader(layer(t, headers, contents)), &n)
	}
	// Hardlinks name archive paths (an absolute host path is rejected); sparse files are refused.
	for _, kind := range []byte{tar.TypeLink, tar.TypeGNUSparse} {
		mustReject(t, func() { apply(t.TempDir(), []tar.Header{{Name: "bad", Typeflag: kind, Linkname: "/tmp/outside"}}, nil) })
	}
	root := t.TempDir()
	apply(root, []tar.Header{{Name: "dev/null", Typeflag: tar.TypeChar}, {Name: "fifo", Typeflag: tar.TypeFifo}}, nil) // skipped
	if _, e := os.Lstat(filepath.Join(root, "fifo")); e == nil {
		t.Fatal("FIFO extracted")
	}
	apply(root, []tar.Header{{Name: "a", Size: 3, Mode: 0644}, {Name: "b", Size: 1, Mode: 0644}, {Name: "dir/old", Size: 1, Mode: 0644}}, []string{"old", "b", "o"})
	// Same-layer whiteouts hide lower layers only; an opaque directory drops its lower files.
	apply(root, []tar.Header{{Name: "a", Size: 3, Mode: 0700}, {Name: ".wh.a"}, {Name: ".wh.b"}, {Name: "dir/new", Size: 1}, {Name: "dir/.wh..wh..opq"}}, []string{"new", "", "", "x", ""})
	if data, _ := os.ReadFile(filepath.Join(root, "a")); string(data) != "new" {
		t.Fatal("whiteout removed upper layer")
	}
	if st, _ := os.Stat(filepath.Join(root, "a")); st.Mode().Perm() != 0700 {
		t.Fatalf("mode not kept: %v", st.Mode())
	}
	for _, gone := range []string{"b", "dir/old"} {
		if _, e := os.Lstat(filepath.Join(root, gone)); e == nil {
			t.Fatal("whiteout missed " + gone)
		}
	}
	if _, e := os.Lstat(filepath.Join(root, "dir/new")); e != nil {
		t.Fatal("opaque whiteout removed this layer's file")
	}
	// Nothing is written through a symlink.
	mustReject(t, func() {
		apply(t.TempDir(), []tar.Header{{Name: "lib", Typeflag: tar.TypeSymlink, Linkname: "/tmp"}, {Name: "lib/x", Size: 1}}, []string{"", "x"})
	})
}
func TestJSONAndProcessLimits(t *testing.T) {
	for _, raw := range []string{`{"a":1,"a":2}`, `[]`, `null`, `{"a":NaN}`, `{"a":true} trailing`, string([]byte{'{', '"', 'a', '"', ':', '"', 255, '"', '}'})} {
		mustReject(t, func() { decode([]byte(raw), maxJSON) })
	}
	mustReject(t, func() { decode([]byte(`{"a":1}`), 2) })
	mustReject(t, func() { validateEnv([]string{"X=1", "X=2"}) })
	mustReject(t, func() { validateEnv([]string{"=bad"}) })
}
func TestPinnedImagesAndImport(t *testing.T) {
	for _, relative := range []string{"examples/m19/shell-image", "examples/m11_17/http-image", "examples/redis-preview/image"} {
		t.Run(relative, func(t *testing.T) {
			root := filepath.Join("..", relative)
			im := loadImage(root)
			cache := filepath.Join(t.TempDir(), "cache")
			target := commitImage(cache, "fixture", root)
			if again := loadImage(target); im.Digest != again.Digest {
				t.Fatal("import changed digest")
			}
			unpacked := filepath.Join(filepath.Dir(target), "rootfs")
			if size := readObject(filepath.Join(filepath.Dir(target), "image.json"))["size"]; integer(size) <= 0 {
				t.Fatalf("unpacked size: %v", size)
			}
			if _, e := os.Stat(filepath.Join(unpacked, "dev")); e != nil {
				t.Fatal("no /dev")
			}
			if commitImage(cache, "fixture", root) != target {
				t.Fatal("idempotent import")
			}
			// A tag clones the unpacked root.
			tagged := commitImage(cache, "fixture:tag", target)
			if _, e := os.Stat(filepath.Join(filepath.Dir(tagged), "rootfs", "dev")); e != nil {
				t.Fatal("tag has no unpacked root")
			}
			// A corrupted layer fails unpacking.
			layerDigest := str(im.layers[len(im.layers)-1]["digest"])
			p := filepath.Join(target, "blobs/sha256", layerDigest[7:])
			data, e := os.ReadFile(p)
			check(e)
			check(os.Remove(p))
			data[len(data)/2] ^= 1
			check(os.WriteFile(p, data, 0600))
			check(os.RemoveAll(unpacked))
			mustReject(t, func() { ensureUnpacked(filepath.Dir(target)) })
		})
	}
}
func TestSymlinkAndPrivateState(t *testing.T) {
	root := t.TempDir()
	check(os.Chmod(root, 0700))
	link := filepath.Join(root, "link")
	check(os.Symlink(t.TempDir(), link))
	mustReject(t, func() { private(link) })
	mustReject(t, func() { noLinks(link, "anything") })
	check(os.WriteFile(filepath.Join(root, "file"), []byte("x"), 0600))
	check(os.Symlink("file", filepath.Join(root, "other")))
	mustReject(t, func() { readBounded(root, "other", 100) })
	check(os.Chmod(root, 0755))
	mustReject(t, func() { private(root) })
}
func testEngine(t *testing.T) *engine {
	t.Helper()
	home := t.TempDir()
	check(os.Chmod(home, 0700))
	return engineAt(home, home)
}

// engineAt is a test engine whose images and containers are in root: home, or its
// store (store_test.go).
func engineAt(home, root string) *engine {
	b := &engine{Home: home, State: private(filepath.Join(root, "containers")), Cache: private(filepath.Join(root, "images")), Assets: "..", Native: "../build/dockiva-runtime", MaxContainers: 64}
	commitImage(b.Cache, "dockiva/shell:demo", "../examples/m19/shell-image")
	// Hermetic default: the host's hypervisor availability must not change test results.
	nativeProbe.Lock()
	nativeProbe.known[b.Native] = false
	nativeProbe.Unlock()
	return b
}
func request(t *testing.T, h http.Handler, method, path, body, secret string) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, path, strings.NewReader(body))
	if secret != "" {
		r.Header.Set("X-VMFree-Token", secret)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	return w
}
func TestAPIContractAndRejectedOptions(t *testing.T) {
	b := testEngine(t)
	h := b.handler("secret")
	if request(t, h, "GET", "/_ping", "", "").Code != 401 {
		t.Fatal("missing auth accepted")
	}
	v := request(t, h, "GET", "/version", "", "secret")
	var version struct {
		GitCommit  string
		Components []struct {
			Name    string
			Details map[string]string
		}
	}
	if v.Code != 200 || json.Unmarshal(v.Body.Bytes(), &version) != nil {
		t.Fatal("version")
	}
	commit, _ := buildStamp() // test binaries carry no vcs stamp: "unknown"
	if version.GitCommit != commit || len(version.Components) == 0 || version.Components[0].Name != "Engine" || version.Components[0].Details["GitCommit"] != commit {
		t.Fatalf("version commit: %s", v.Body.String())
	}
	if request(t, h, "GET", "/v1.99/info", "", "secret").Code != 400 {
		t.Fatal("version range")
	}
	for _, body := range []string{`{"Image":"dockiva/shell:demo","HostConfig":{"Privileged":true}}`, `{"Image":"dockiva/shell:demo","HostConfig":{"Binds":["/tmp:/tmp:shared"]}}`, `{"Image":"dockiva/shell:demo","NetworkingConfig":{"EndpointsConfig":{"overlay":{}}}}`, `{"Image":"dockiva/shell:demo","Env":["bad"]}`, `{"Image":"dockiva/shell:demo","Image":"duplicate"}`} {
		r := request(t, h, "POST", "/containers/create?name=bad", body, "secret")
		if r.Code < 400 || (r.Code >= 500 && r.Code != 501) {
			t.Fatalf("bad option: %d %s", r.Code, r.Body.String())
		}
	}
	r := request(t, h, "POST", "/containers/create?name=valid", `{"Image":"dockiva/shell:demo","Cmd":["-c","busybox echo go"],"Labels":{"test":"go"},"HostConfig":{"ReadonlyRootfs":true}}`, "secret")
	if r.Code != 201 {
		t.Fatalf("create: %d %s", r.Code, r.Body.String())
	}
	if request(t, h, "POST", "/containers/create?name=valid", `{"Image":"dockiva/shell:demo"}`, "secret").Code != 409 {
		t.Fatal("duplicate name")
	}
	for _, path := range []string{"/containers/valid/json", "/containers/json?all=1&filters=%7B%22label%22:%5B%22test=go%22%5D%7D", "/images/json", "/images/dockiva/shell:demo/json"} {
		r := request(t, h, "GET", path, "", "secret")
		if r.Code != 200 {
			t.Fatalf("%s: %d %s", path, r.Code, r.Body.String())
		}
	}
	// A stopped container: follow returns the existing logs and ends.
	if r := request(t, h, "GET", "/containers/valid/logs?follow=true&stdout=1", "", "secret"); r.Code != 200 {
		t.Fatalf("logs follow: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "POST", "/containers/valid/wait", "", "secret"); r.Code != 200 || !strings.Contains(r.Body.String(), `"StatusCode":0`) {
		t.Fatalf("wait on a created container: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "POST", "/containers/valid/kill?signal=TERM", "", "secret"); r.Code != 409 {
		t.Fatalf("kill of a stopped container: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "POST", "/containers/valid/kill?signal=SIGSTOP", "", "secret"); r.Code != 400 {
		t.Fatalf("unsupported signal: %d %s", r.Code, r.Body.String())
	}
	if request(t, h, "DELETE", "/containers/valid", "", "secret").Code != 204 {
		t.Fatal("remove")
	}
	// docker run -it: a terminal and standard input, shown by inspect.
	if r := request(t, h, "POST", "/containers/create?name=interactive", `{"Image":"dockiva/shell:demo","Tty":true,"OpenStdin":true,"StdinOnce":true,"AttachStdin":true,"HostConfig":{"ConsoleSize":[24,80]}}`, "secret"); r.Code != 201 {
		t.Fatalf("-it: %d %s", r.Code, r.Body.String())
	}
	if config := b.inspect("interactive")["Config"].(obj); config["Tty"] != true || config["OpenStdin"] != true || config["StdinOnce"] != true {
		t.Fatalf("-it inspect: %v", config)
	}
	if r := request(t, h, "POST", "/containers/create?name=once", `{"Image":"dockiva/shell:demo","HostConfig":{"AutoRemove":true}}`, "secret"); r.Code != 201 {
		t.Fatalf("--rm: %d %s", r.Code, r.Body.String())
	}
	if !strings.Contains(request(t, h, "GET", "/containers/once/json", "", "secret").Body.String(), `"AutoRemove":true`) {
		t.Fatal("AutoRemove not reported")
	}
	// The user-only Docker socket's handler needs no token; the engine socket's does.
	if request(t, b.socketHandler(), "GET", "/containers/json?all=1", "", "").Code != 200 || request(t, h, "GET", "/containers/json", "", "").Code != 401 {
		t.Fatal("socket authorization")
	}
}
func TestRegistryVerificationAndRedirect(t *testing.T) {
	root := "../examples/m19/shell-image"
	idx := readObject(filepath.Join(root, "index.json"))
	desc := object(array(idx["manifests"])[0])
	manifest := readBounded(root, "blobs/sha256/"+str(desc["digest"])[7:], maxJSON)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.Contains(r.URL.Path, "/manifests/") {
			_, _ = w.Write(manifest)
			return
		}
		d := filepath.Base(r.URL.Path)
		if !digestPattern.MatchString(d) {
			http.Error(w, "bad", 400)
			return
		}
		_, _ = w.Write(readBounded(root, "blobs/sha256/"+d[7:], maxBlob))
	}))
	defer server.Close()
	reference := strings.TrimPrefix(server.URL, "http://") + "/test/shell:demo"
	cache := filepath.Join(t.TempDir(), "cache")
	target := pullImage(reference, cache, true, "")
	if loadImage(target).Digest != desc["digest"] {
		t.Fatal("pull changed digest")
	}
	mustReject(t, func() {
		pullImage(strings.TrimSuffix(reference, "demo")+"sha256:"+strings.Repeat("0", 64), cache, true, "")
	})
	// Blobs served through a CDN-style redirect are followed without the registry token.
	leaked := false
	cdn := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		leaked = leaked || r.Header.Get("Authorization") != ""
		_, _ = w.Write(readBounded(root, "blobs/sha256/"+filepath.Base(r.URL.Path), maxBlob))
	}))
	defer cdn.Close()
	redirect := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.Contains(r.URL.Path, "/manifests/") {
			_, _ = w.Write(manifest)
			return
		}
		http.Redirect(w, r, cdn.URL+"/"+strings.TrimPrefix(filepath.Base(r.URL.Path), "sha256:"), 307)
	}))
	defer redirect.Close()
	redirected := pullImage(strings.TrimPrefix(redirect.URL, "http://")+"/test/redirected:demo", cache, true, "secret")
	if loadImage(redirected).Digest != desc["digest"] {
		t.Fatal("redirected pull changed digest")
	}
	if leaked {
		t.Fatal("registry token forwarded to redirect target")
	}
	// A redirect target that serves different bytes still fails digest verification.
	evil := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write([]byte("evil")) }))
	defer evil.Close()
	tampered := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.Contains(r.URL.Path, "/manifests/") {
			_, _ = w.Write(manifest)
			return
		}
		http.Redirect(w, r, evil.URL, 307)
	}))
	defer tampered.Close()
	mustReject(t, func() {
		pullImage(strings.TrimPrefix(tampered.URL, "http://")+"/test/tampered:demo", cache, true, "")
	})
}

// docker pull of a tag the registry has moved to another image (alpine:latest after
// an update) moves the cached name, as docker tag does; a container on the old image
// keeps its files.
func TestPullMovesUpdatedTag(t *testing.T) {
	roots := []string{"../examples/m19/shell-image", "../examples/m11_17/http-image"}
	digests := []string{}
	for _, root := range roots {
		digests = append(digests, str(object(array(readObject(filepath.Join(root, "index.json"))["manifests"])[0])["digest"]))
	}
	current := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.Contains(r.URL.Path, "/manifests/") {
			_, _ = w.Write(readBounded(roots[current], "blobs/sha256/"+digests[current][7:], maxJSON))
			return
		}
		d := filepath.Base(r.URL.Path)
		if !digestPattern.MatchString(d) {
			http.Error(w, "bad", 400)
			return
		}
		for _, root := range roots {
			if exists(filepath.Join(root, "blobs/sha256", d[7:])) {
				_, _ = w.Write(readBounded(root, "blobs/sha256/"+d[7:], maxBlob))
				return
			}
		}
		http.NotFound(w, r)
	}))
	defer server.Close()
	b := testEngine(t)
	b.Insecure = true
	h := b.handler("secret")
	name := strings.TrimPrefix(server.URL, "http://") + "/test/app:v1"
	pull := func(want string) {
		t.Helper()
		if r := request(t, h, "POST", "/images/create?fromImage="+strings.TrimSuffix(name, ":v1")+"&tag=v1", "", "secret"); r.Code != 200 {
			t.Fatalf("pull: %d %s", r.Code, r.Body.String())
		}
		if got := text(readObject(filepath.Join(b.imagePath(name), "image.json"))["digest"]); got != want {
			t.Fatalf("%s is %s, want %s", name, got, want)
		}
	}
	pull(digests[0])
	if r := request(t, h, "POST", "/containers/create?name=old", `{"Image":"`+name+`"}`, "secret"); r.Code != 201 {
		t.Fatalf("create: %d %s", r.Code, r.Body.String())
	}
	pull(digests[0]) // unchanged: a no-op
	current = 1
	pull(digests[1])
	lower := text(readObject(filepath.Join(b.State, b.resolve("old"), "config.json"))["lower"])
	if lower == "" || !exists(lower) {
		t.Fatalf("the old image's files went with its tag: %q", lower)
	}
	// docker tag onto a taken name moves it too.
	if r := request(t, h, "POST", "/images/dockiva/shell:demo/tag?repo="+strings.TrimSuffix(name, ":v1")+"&tag=v1", "", "secret"); r.Code != 201 {
		t.Fatalf("tag: %d %s", r.Code, r.Body.String())
	}
	if got := text(readObject(filepath.Join(b.imagePath(name), "image.json"))["digest"]); got != digests[0] {
		t.Fatalf("tag did not move %s: %s", name, got)
	}
}

func TestParseReference(t *testing.T) {
	for in, want := range map[string][4]string{
		"alpine":                         {"registry-1.docker.io", "library/alpine", "latest", "alpine:latest"},
		"alpine:3.20":                    {"registry-1.docker.io", "library/alpine", "3.20", "alpine:3.20"},
		"bitnami/redis:7.4":              {"registry-1.docker.io", "bitnami/redis", "7.4", "bitnami/redis:7.4"},
		"docker.io/library/nginx:alpine": {"registry-1.docker.io", "library/nginx", "alpine", "nginx:alpine"},
		"docker.io/nginx":                {"registry-1.docker.io", "library/nginx", "latest", "nginx:latest"},
		"ghcr.io/owner/app:v1":           {"ghcr.io", "owner/app", "v1", "ghcr.io/owner/app:v1"},
		"localhost/app":                  {"localhost", "app", "latest", "localhost/app:latest"},
		"127.0.0.1:5000/app:dev":         {"127.0.0.1:5000", "app", "dev", "127.0.0.1:5000/app:dev"},
	} {
		host, repo, ref, name := parseReference(in)
		if got := [4]string{host, repo, ref, name}; got != want {
			t.Errorf("%s: got %v, want %v", in, got, want)
		}
	}
	mustReject(t, func() { parseReference("Alpine") })
}
func TestLegacyStateAndStaleOwnership(t *testing.T) {
	b := testEngine(t)
	b.create("legacy", decode([]byte(`{"Image":"dockiva/shell:demo","HostConfig":{"ReadonlyRootfs":true}}`), maxJSON))
	p := b.container("legacy")
	save(filepath.Join(p, "status.json"), obj{"state": "running", "generation": "old-python-generation", "pid": 99999999, "started_at": now()})
	state := object(b.inspect("legacy")["State"])
	if state["Running"] != false || state["Status"] != "dead" || state["ExitCode"] != 125 {
		t.Fatalf("stale state: %v", state)
	}
	if !strings.Contains(text(state["Error"]), "ownership lost") {
		t.Fatal("missing recovery explanation")
	}
	b.remove("legacy")
}

func TestUnpublishedCreationRecovery(t *testing.T) {
	b := testEngine(t)
	b.create("retained", decode([]byte(`{"Image":"dockiva/shell:demo"}`), maxJSON))
	stage := filepath.Join(b.State, ".creating-interrupted")
	check(os.MkdirAll(filepath.Join(stage, "rootfs"), 0700))
	check(os.WriteFile(filepath.Join(stage, "rootfs", "partial"), []byte("partial"), 0600))
	cleanupUnpublished(b.State)
	if exists(stage) || !exists(filepath.Join(b.State, "retained", "status.json")) {
		t.Fatal("recovery must reclaim unpublished state and retain committed containers")
	}
}

func TestLayerLinks(t *testing.T) {
	elf := "\x7fELF\x02\x01\x01busybox"
	root, count := t.TempDir(), 0
	applyLayer(root, bytes.NewReader(layer(t, []tar.Header{
		{Name: "bin/", Typeflag: tar.TypeDir, Mode: 0755},
		{Name: "bin/busybox", Size: int64(len(elf)), Mode: 0755},
		{Name: "bin/sh", Typeflag: tar.TypeSymlink, Linkname: "busybox"},
		{Name: "bin/ls", Typeflag: tar.TypeLink, Linkname: "bin/busybox"},
		{Name: "usr/bin/env", Typeflag: tar.TypeSymlink, Linkname: "../../bin/busybox"},
		{Name: "host", Typeflag: tar.TypeSymlink, Linkname: "/etc/passwd"},
	}, []string{"", elf})), &count)
	if target, e := os.Readlink(filepath.Join(root, "host")); e != nil || target != "/etc/passwd" {
		t.Fatalf("symlink extraction: %q %v", target, e)
	}
	a, e1 := os.Stat(filepath.Join(root, "bin/busybox"))
	b, e2 := os.Stat(filepath.Join(root, "bin/ls"))
	if e1 != nil || e2 != nil || !os.SameFile(a, b) {
		t.Fatal("hardlink extraction did not share the inode")
	}
	// Executables resolve through chained, relative and absolute links.
	im := &image{Root: root, Entry: []string{"/bin/sh"}}
	if cmd := im.command(nil); cmd[0] != "/bin/sh" {
		t.Fatalf("command rewritten: %v", cmd)
	}
	if cmd := (&image{Root: root}).command([]string{"sh", "-c", "true"}); cmd[0] != "/bin/sh" || cmd[1] != "-c" {
		t.Fatalf("PATH lookup: %v", cmd)
	}
	if cmd := (&image{Root: root, Env: []string{"PATH=/usr/bin"}, Entry: []string{"env"}}).command(nil); cmd[0] != "/usr/bin/env" {
		t.Fatalf("image PATH lookup: %v", cmd)
	}
	mustReject(t, func() { (&image{Root: root, Entry: []string{"missing"}}).command(nil) })
	if cmd := (&image{Root: root, WorkingDir: "/usr"}).command([]string{"./bin/env"}); cmd[0] != "/usr/bin/env" {
		t.Fatalf("relative to WORKDIR: %v", cmd)
	}
	link := func(p string) (string, bool) { return readLink(filepath.Join(root, p)) }
	for in, want := range map[string]string{"bin/sh": "bin/busybox", "usr/bin/env": "bin/busybox", "../../../bin/sh": "bin/busybox", "host": "etc/passwd", "bin/../usr": "usr"} {
		if got := resolveGuest(in, link); got != want {
			t.Errorf("resolveGuest(%q) = %q, want %q", in, got, want)
		}
	}
	mustReject(t, func() { resolveGuest("loop", func(string) (string, bool) { return "loop", true }) })
	// A later layer whiteouts the shell link.
	applyLayer(root, bytes.NewReader(layer(t, []tar.Header{{Name: "bin/.wh.sh"}}, nil)), &count)
	if _, e := os.Lstat(filepath.Join(root, "bin/sh")); e == nil {
		t.Fatal("whiteout missed symlink")
	}
	mustReject(t, func() {
		applyLayer(t.TempDir(), bytes.NewReader(layer(t, []tar.Header{{Name: "ls", Typeflag: tar.TypeLink, Linkname: "missing"}}, nil)), &count)
	})
}
func TestScriptEntrypointAndDirectories(t *testing.T) {
	root := t.TempDir()
	files := map[string]string{"bin/sh": "\x7fELF\x02\x01\x01sh", "entry.sh": "#!/bin/sh\nexec \"$@\"\n", "with-arg": "#! /usr/bin/env  sh -x \r\n",
		"nested": "#!/entry.sh\n", "plain": "echo no interpreter\n"}
	for name, data := range files {
		check(os.MkdirAll(filepath.Dir(filepath.Join(root, name)), 0755))
		check(os.WriteFile(filepath.Join(root, name), []byte(data), 0755))
	}
	check(os.MkdirAll(filepath.Join(root, "usr/bin"), 0755))
	check(os.Symlink("/bin/sh", filepath.Join(root, "usr/bin/env")))
	check(os.MkdirAll(filepath.Join(root, "var"), 0755))
	check(os.MkdirAll(filepath.Join(root, "srv"), 0755))
	check(os.Symlink("/srv", filepath.Join(root, "var/lib")))
	check(os.WriteFile(filepath.Join(root, "file"), []byte("x"), 0644))
	im := &image{Root: root}
	got := im.command([]string{"/entry.sh", "redis-server"})
	if strings.Join(got, " ") != "/bin/sh /entry.sh redis-server" {
		t.Fatalf("script: %v", got)
	}
	if got = im.command([]string{"/with-arg", "a"}); strings.Join(got, "|") != "/usr/bin/env|sh -x|/with-arg|a" {
		t.Fatalf("interpreter argument: %q", got)
	}
	if got = im.command([]string{"/nested", "x"}); strings.Join(got, " ") != "/bin/sh /entry.sh /nested x" { // a script interpreter
		t.Fatalf("nested interpreter: %q", got)
	}
	mustReject(t, func() { im.command([]string{"/plain"}) }) // not ELF, no #!
	mustReject(t, func() { im.command([]string{"/file"}) })  // not executable
	if im.ensureDir("/data") != "/data" {
		t.Fatal("ensureDir result")
	}
	if st, e := os.Stat(filepath.Join(root, "data")); e != nil || !st.IsDir() {
		t.Fatal("WorkingDir/volume not created")
	}
	im.ensureDir("/var/lib/redis") // through a symlink: lands in /srv
	if st, e := os.Stat(filepath.Join(root, "srv/redis")); e != nil || !st.IsDir() {
		t.Fatal("symlinked parent not followed")
	}
	mustReject(t, func() { im.ensureDir("/file/sub") })
	mustReject(t, func() { im.ensureDir("relative") })
}
func TestNetworkModeAndDNSFiles(t *testing.T) {
	b := testEngine(t)
	for name, mode := range map[string]string{"plain": "", "bridge": `"bridge"`, "host": `"host"`, "none": `"none"`} {
		body := `{"Image":"dockiva/shell:demo"}`
		if mode != "" {
			body = `{"Image":"dockiva/shell:demo","HostConfig":{"NetworkMode":` + mode + `}}`
		}
		b.create(name, decode([]byte(body), maxJSON))
		want := map[bool]string{true: "none", false: "outbound"}[name == "none"]
		if got := readObject(filepath.Join(b.State, name, "config.json"))["network"]; got != want {
			t.Fatalf("%s: network %v", name, got)
		}
	}
	mustReject(t, func() {
		b.create("overlay", decode([]byte(`{"Image":"dockiva/shell:demo","HostConfig":{"NetworkMode":"overlay"}}`), maxJSON))
	})
	host := "# macOS\nnameserver 192.168.1.1\nnameserver fe80::1%en0\nnameserver 127.0.0.1\nsearch lan example.com\n"
	if got := resolvConf([]byte(host)); got != "nameserver 192.168.1.1\nnameserver 127.0.0.1\nsearch lan example.com\n" {
		t.Fatalf("resolv.conf: %q", got)
	}
	if got := resolvConf(nil); got != "nameserver 8.8.8.8\nnameserver 8.8.4.4\n" {
		t.Fatalf("fallback: %q", got)
	}
	// A guest symlink is replaced, never followed out of the root.
	root, outside := t.TempDir(), filepath.Join(t.TempDir(), "victim")
	check(os.WriteFile(outside, []byte("keep"), 0600))
	check(os.MkdirAll(filepath.Join(root, "etc"), 0755))
	check(os.Symlink(outside, filepath.Join(root, "etc", "hosts")))
	networkFiles(layers{upper: root}, hostsFile("", "web", obj{}, nil))
	if data, _ := os.ReadFile(outside); string(data) != "keep" {
		t.Fatal("symlink followed")
	}
	if data, _ := os.ReadFile(filepath.Join(root, "etc", "hosts")); !strings.Contains(string(data), "127.0.0.1\tweb\n") || !strings.Contains(string(data), "host.docker.internal") {
		t.Fatalf("hosts: %q", data)
	}
	link := t.TempDir()
	check(os.Symlink(t.TempDir(), filepath.Join(link, "etc")))
	mustReject(t, func() { networkFiles(layers{upper: link}, "") }) // a symlinked /etc is refused
}

func TestShortName(t *testing.T) {
	for in, want := range map[string]string{"docker.io/library/nginx:alpine": "nginx:alpine", "docker.io/grafana/grafana:latest": "grafana/grafana:latest", "nginx": "nginx", "ghcr.io/a/b:1": "ghcr.io/a/b:1"} {
		if got := shortName(in); got != want {
			t.Fatalf("%s: %s", in, got)
		}
	}
}

func TestResourceLimits(t *testing.T) {
	b := testEngine(t)
	b.create("small", decode([]byte(`{"Image":"dockiva/shell:demo","HostConfig":{"Memory":104857601,"MemorySwap":-1,"NanoCpus":1500000000}}`), maxJSON))
	host := b.inspect("small")["HostConfig"].(obj)
	if host["Memory"] != int64(101<<20) || host["NanoCpus"] != int64(2e9) {
		t.Fatalf("limits: %v %v", host["Memory"], host["NanoCpus"])
	}
	// --memory-swap: unset is as much swap as -m (Docker on a host with swap), -1 unlimited,
	// else memory plus swap; never below -m.
	for body, want := range map[string]int64{`"Memory":104857600`: 200 << 20, `"Memory":104857600,"MemorySwap":0`: 200 << 20,
		`"Memory":104857600,"MemorySwap":-1`: -1, `"Memory":104857600,"MemorySwap":104857600`: 100 << 20, `"Memory":104857600,"MemorySwap":314572800`: 300 << 20} {
		b.create("swap", decode([]byte(`{"Image":"dockiva/shell:demo","HostConfig":{`+body+`}}`), maxJSON))
		if got := b.inspect("swap")["HostConfig"].(obj)["MemorySwap"]; got != want {
			t.Fatalf("%s: MemorySwap %v, want %d", body, got, want)
		}
		b.remove("swap")
	}
	// The docker CLI sends zeros when -m and --cpus are absent: the profile's limits apply.
	b.create("plain", decode([]byte(`{"Image":"dockiva/shell:demo","HostConfig":{"Memory":0,"NanoCpus":0,"MemorySwap":0}}`), maxJSON))
	if host := b.inspect("plain")["HostConfig"].(obj); host["Memory"] == int64(0) || host["NanoCpus"] != int64(0) || host["MemorySwap"] != int64(0) {
		t.Fatalf("defaults: %v %v %v", host["Memory"], host["NanoCpus"], host["MemorySwap"])
	}
	for _, bad := range []string{`{"Memory":1024}`, `{"Memory":70000000000000}`, `{"NanoCpus":65000000000}`, `{"MemorySwap":-2}`, `{"Memory":104857600,"MemorySwap":52428800}`} {
		mustReject(t, func() {
			b.create("bad", decode([]byte(`{"Image":"dockiva/shell:demo","HostConfig":`+bad+`}`), maxJSON))
		})
	}
}

func TestContainerCommandScripts(t *testing.T) {
	p := t.TempDir()
	root := filepath.Join(p, "rootfs")
	check(os.MkdirAll(filepath.Join(root, "bin"), 0o755))
	check(os.WriteFile(filepath.Join(root, "bin", "sh"), append([]byte("\x7fELF\x02\x01\x01"), make([]byte, 57)...), 0o755))
	check(os.WriteFile(filepath.Join(root, "bin", "tool.sh"), []byte("#!/bin/sh\necho hi\n"), 0o755))
	cmd, argv0 := containerCommand(p, obj{}, "/", []string{"PATH=/bin"}, []string{"tool.sh", "--list"})
	if strings.Join(cmd, " ") != "/bin/sh /bin/tool.sh --list" || argv0 != "/bin/sh" {
		t.Fatalf("script: %v %q", cmd, argv0)
	}
	cmd, argv0 = containerCommand(p, obj{}, "/", []string{"PATH=/bin"}, []string{"sh", "-c", "true"})
	if strings.Join(cmd, " ") != "/bin/sh -c true" || argv0 != "sh" {
		t.Fatalf("binary: %v %q", cmd, argv0)
	}
}

// docker logs -t, --since and --until read each line's time from the index the
// supervisor's stampedLog writes; a log without one takes its file's mtime.
func TestTimestampedLogs(t *testing.T) {
	b := testEngine(t)
	h := b.handler("secret")
	if r := request(t, h, "POST", "/containers/create?name=logs", `{"Image":"dockiva/shell:demo"}`, "secret"); r.Code != 201 {
		t.Fatalf("create: %d %s", r.Code, r.Body.String())
	}
	p := b.container("logs")
	// The writer: an entry for each line start, partial lines across writes included.
	l := openStampedLog(p, "stdout")
	for _, s := range []string{"one\ntw", "o\n", "three\n"} {
		_, e := l.Write([]byte(s))
		check(e)
	}
	check(l.Close())
	idx, e := os.ReadFile(filepath.Join(p, "stdout.idx"))
	check(e)
	if len(idx) != 3*16 || binary.LittleEndian.Uint64(idx[16:]) != 4 || binary.LittleEndian.Uint64(idx[32:]) != 8 {
		t.Fatalf("index: %v", idx)
	}
	// Known times: lines at 1000 s, 2000 s and 3000 s.
	for i, at := range []uint64{1000, 2000, 3000} {
		binary.LittleEndian.PutUint64(idx[i*16+8:], at*1e9+5)
	}
	check(os.WriteFile(filepath.Join(p, "stdout.idx"), idx, 0600))
	logs := func(query string) string {
		r := request(t, h, "GET", "/containers/logs/logs?stdout=1&"+query, "", "secret")
		if r.Code != 200 {
			t.Fatalf("%s: %d %s", query, r.Code, r.Body.String())
		}
		out := ""
		for raw := r.Body.Bytes(); len(raw) >= 8; raw = raw[8+binary.BigEndian.Uint32(raw[4:]):] {
			out += string(raw[8 : 8+binary.BigEndian.Uint32(raw[4:])])
		}
		return out
	}
	for query, want := range map[string]string{
		"timestamps=1":                      "1970-01-01T00:16:40.000000005Z one\n1970-01-01T00:33:20.000000005Z two\n1970-01-01T00:50:00.000000005Z three\n",
		"since=1500":                        "two\nthree\n",
		"since=2000.000000005&until=2000.5": "two\n",
		"until=1970-01-01T00:20:00Z":        "one\n",
		"timestamps=1&tail=1":               "1970-01-01T00:50:00.000000005Z three\n",
		"since=0&until=0":                   "one\ntwo\nthree\n",
	} {
		if got := logs(query); got != want {
			t.Errorf("%s: %q, want %q", query, got, want)
		}
	}
	// A log from before indexes: its lines take the log's last write.
	check(os.Remove(filepath.Join(p, "stdout.idx")))
	when := time.Unix(4000, 0)
	check(os.Chtimes(filepath.Join(p, "stdout.log"), when, when))
	if got := logs("timestamps=1&tail=1"); got != "1970-01-01T01:06:40.000000000Z three\n" {
		t.Errorf("unindexed: %q", got)
	}
	if r := request(t, h, "GET", "/containers/logs/logs?stdout=1&since=yesterday", "", "secret"); r.Code != 400 {
		t.Errorf("bad since: %d", r.Code)
	}
}
