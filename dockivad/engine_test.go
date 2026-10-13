package main

import (
	"archive/tar"
	"bytes"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
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
	for _, name := range []string{"../escape", "/escape", "a/../../escape", "a\\b", "a:b", "a/\x01b"} {
		t.Run(name, func(t *testing.T) { mustReject(t, func() { pathName(name) }) })
	}
	// Hardlinks name archive paths, so an absolute host path is rejected; an
	// absolute symlink target is a guest path (see TestLayerLinks).
	for _, kind := range []byte{tar.TypeLink, tar.TypeChar, tar.TypeFifo, tar.TypeGNUSparse} {
		t.Run(string(kind), func(t *testing.T) {
			mustReject(t, func() {
				count := 0
				applyLayer(map[string]entry{}, layer(t, []tar.Header{{Name: "bad", Typeflag: kind, Linkname: "/tmp/outside"}}, nil), &count)
			})
		})
	}
	tree := map[string]entry{"a": {Data: []byte("old")}, "b": {}, "dir": {Dir: true}, "dir/old": {}}
	count := 0
	applyLayer(tree, layer(t, []tar.Header{{Name: "a", Size: 3, Mode: 0700}, {Name: ".wh.a"}, {Name: ".wh.b"}, {Name: "dir/new", Size: 1}, {Name: "dir/.wh..wh..opq"}}, []string{"new", "", "", "x", ""}), &count)
	if string(tree["a"].Data) != "new" {
		t.Fatal("whiteout removed upper layer")
	}
	if _, ok := tree["b"]; ok {
		t.Fatal("whiteout missed")
	}
	if _, ok := tree["dir/old"]; ok {
		t.Fatal("opaque whiteout missed")
	}
	for _, names := range [][]string{{"a", "a"}, {"a", "A"}, {"a", "a/b"}} {
		mustReject(t, func() {
			n := 0
			applyLayer(map[string]entry{}, layer(t, []tar.Header{{Name: names[0]}, {Name: names[1]}}, nil), &n)
		})
	}
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
			again := loadImage(target)
			if im.Digest != again.Digest {
				t.Fatal("import changed digest")
			}
			out := t.TempDir()
			im.extract(out)
			for name, v := range im.Tree {
				st, e := os.Stat(filepath.Join(out, name))
				if e != nil {
					t.Fatal(e)
				}
				if !v.Dir && st.Size() != int64(len(v.Data)) {
					t.Fatal("extraction size mismatch")
				}
			}
			if commitImage(cache, "fixture", root) != target {
				t.Fatal("idempotent import")
			}
			for d, raw := range im.Blobs {
				if len(raw) > 0 {
					bad := append([]byte{}, raw...)
					bad[0] ^= 1
					check(os.WriteFile(filepath.Join(target, "blobs/sha256", d[7:]), bad, 0600))
					break
				}
			}
			mustReject(t, func() { loadImage(target) })
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
	b := &engine{Home: home, State: private(filepath.Join(home, "containers")), Cache: private(filepath.Join(home, "images")), Assets: "..", Native: "../build/vmfree-c", MaxContainers: 64}
	commitImage(b.Cache, "vmfree/shell:demo", "../examples/m19/shell-image")
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
	if request(t, h, "GET", "/version", "", "secret").Code != 200 {
		t.Fatal("version")
	}
	if request(t, h, "GET", "/v1.99/info", "", "secret").Code != 400 {
		t.Fatal("version range")
	}
	for _, body := range []string{`{"Image":"vmfree/shell:demo","HostConfig":{"Privileged":true}}`, `{"Image":"vmfree/shell:demo","HostConfig":{"Binds":["/tmp:/tmp:shared"]}}`, `{"Image":"vmfree/shell:demo","NetworkingConfig":{"EndpointsConfig":{"overlay":{}}}}`, `{"Image":"vmfree/shell:demo","Cmd":[]}`, `{"Image":"vmfree/shell:demo","Env":["bad"]}`, `{"Image":"vmfree/shell:demo","Image":"duplicate"}`} {
		r := request(t, h, "POST", "/containers/create?name=bad", body, "secret")
		if r.Code < 400 || (r.Code >= 500 && r.Code != 501) {
			t.Fatalf("bad option: %d %s", r.Code, r.Body.String())
		}
	}
	r := request(t, h, "POST", "/containers/create?name=valid", `{"Image":"vmfree/shell:demo","Cmd":["-c","busybox echo go"],"Labels":{"test":"go"},"HostConfig":{"ReadonlyRootfs":true}}`, "secret")
	if r.Code != 201 {
		t.Fatalf("create: %d %s", r.Code, r.Body.String())
	}
	if request(t, h, "POST", "/containers/create?name=valid", `{"Image":"vmfree/shell:demo"}`, "secret").Code != 409 {
		t.Fatal("duplicate name")
	}
	for _, path := range []string{"/containers/valid/json", "/containers/json?all=1&filters=%7B%22label%22:%5B%22test=go%22%5D%7D", "/images/json", "/images/vmfree/shell:demo/json"} {
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
	for body, message := range map[string]string{`{"Image":"vmfree/shell:demo","Tty":true}`: "docker run -t", `{"Image":"vmfree/shell:demo","OpenStdin":true}`: "docker run -i"} {
		if r := request(t, h, "POST", "/containers/create?name=interactive", body, "secret"); r.Code != 501 || !strings.Contains(r.Body.String(), message) {
			t.Fatalf("%s: %d %s", body, r.Code, r.Body.String())
		}
	}
	if r := request(t, h, "POST", "/containers/create?name=once", `{"Image":"vmfree/shell:demo","HostConfig":{"AutoRemove":true}}`, "secret"); r.Code != 201 {
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

func TestParseReference(t *testing.T) {
	for in, want := range map[string][4]string{
		"alpine":                         {"registry-1.docker.io", "library/alpine", "latest", "alpine:latest"},
		"alpine:3.20":                    {"registry-1.docker.io", "library/alpine", "3.20", "alpine:3.20"},
		"bitnami/redis:7.4":              {"registry-1.docker.io", "bitnami/redis", "7.4", "bitnami/redis:7.4"},
		"docker.io/library/nginx:alpine": {"registry-1.docker.io", "library/nginx", "alpine", "docker.io/library/nginx:alpine"},
		"docker.io/nginx":                {"registry-1.docker.io", "library/nginx", "latest", "docker.io/nginx:latest"},
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
	b.create("legacy", decode([]byte(`{"Image":"vmfree/shell:demo","HostConfig":{"ReadonlyRootfs":true}}`), maxJSON))
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
	b.create("retained", decode([]byte(`{"Image":"vmfree/shell:demo"}`), maxJSON))
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
	tree, count := map[string]entry{}, 0
	applyLayer(tree, layer(t, []tar.Header{
		{Name: "bin/", Typeflag: tar.TypeDir},
		{Name: "bin/busybox", Size: int64(len(elf)), Mode: 0755},
		{Name: "bin/sh", Typeflag: tar.TypeSymlink, Linkname: "busybox"},
		{Name: "bin/ls", Typeflag: tar.TypeLink, Linkname: "bin/busybox"},
		{Name: "usr/bin/env", Typeflag: tar.TypeSymlink, Linkname: "../../bin/busybox"},
		{Name: "host", Typeflag: tar.TypeSymlink, Linkname: "/etc/passwd"},
	}, []string{"", elf}), &count)
	if tree["bin/sh"].Link != "busybox" || tree["host"].Link != "/etc/passwd" {
		t.Fatal("symlink not retained")
	}
	if tree["bin/ls"].Hardlink != "bin/busybox" || string(tree["bin/ls"].Data) != elf || !tree["bin/ls"].Executable {
		t.Fatal("hardlink not retained")
	}
	// A later layer whiteouts the shell link.
	applyLayer(tree, layer(t, []tar.Header{{Name: "bin/.wh.sh"}}, nil), &count)
	if _, ok := tree["bin/sh"]; ok {
		t.Fatal("whiteout missed symlink")
	}
	tree["bin/sh"] = entry{Link: "/usr/bin/env"}

	// Executables resolve through chained, relative and absolute links.
	im := &image{Tree: tree, Entry: []string{"/bin/sh"}}
	if cmd := im.command(nil); cmd[0] != "/bin/sh" {
		t.Fatalf("command rewritten: %v", cmd)
	}
	if cmd := (&image{Tree: tree}).command([]string{"sh", "-c", "true"}); cmd[0] != "/bin/sh" || cmd[1] != "-c" {
		t.Fatalf("PATH lookup: %v", cmd)
	}
	if cmd := (&image{Tree: tree, Env: []string{"PATH=/usr/bin"}, Entry: []string{"env"}}).command(nil); cmd[0] != "/usr/bin/env" {
		t.Fatalf("image PATH lookup: %v", cmd)
	}
	mustReject(t, func() { (&image{Tree: tree, Entry: []string{"missing"}}).command(nil) })
	link := func(p string) (string, bool) { e := tree[p]; return e.Link, e.Link != "" }
	for in, want := range map[string]string{"bin/sh": "bin/busybox", "usr/bin/env": "bin/busybox", "../../../bin/sh": "bin/busybox", "host": "etc/passwd", "bin/../usr": "usr"} {
		if got := resolveGuest(in, link); got != want {
			t.Errorf("resolveGuest(%q) = %q, want %q", in, got, want)
		}
	}
	mustReject(t, func() { resolveGuest("loop", func(string) (string, bool) { return "loop", true }) })

	// Paths beneath a symlink and devices stay rejected.
	mustReject(t, func() {
		applyLayer(map[string]entry{}, layer(t, []tar.Header{{Name: "lib", Typeflag: tar.TypeSymlink, Linkname: "/tmp"}, {Name: "lib/x", Size: 1}}, []string{"", "x"}), &count)
	})
	mustReject(t, func() {
		applyLayer(map[string]entry{}, layer(t, []tar.Header{{Name: "ls", Typeflag: tar.TypeLink, Linkname: "missing"}}, nil), &count)
	})
	mustReject(t, func() { linkTarget("a:b") })

	// Extraction creates real symlinks and shares hardlinked inodes.
	root := t.TempDir()
	im.extract(root)
	if st, e := os.Lstat(filepath.Join(root, "dev")); e != nil || !st.IsDir() {
		t.Fatal("extraction did not provide /dev")
	}
	if target, e := os.Readlink(filepath.Join(root, "host")); e != nil || target != "/etc/passwd" {
		t.Fatalf("symlink extraction: %q %v", target, e)
	}
	a, e1 := os.Stat(filepath.Join(root, "bin/busybox"))
	b, e2 := os.Stat(filepath.Join(root, "bin/ls"))
	if e1 != nil || e2 != nil || !os.SameFile(a, b) {
		t.Fatal("hardlink extraction did not share the inode")
	}
	if got := filepath.Join(root, resolveGuest("bin/sh", func(p string) (string, bool) {
		target, e := os.Readlink(filepath.Join(root, p))
		return target, e == nil
	})); got != filepath.Join(root, "bin/busybox") {
		t.Fatalf("on-disk resolution: %s", got)
	}
}

func TestPublishedPorts(t *testing.T) {
	b := testEngine(t)
	b.create("web", decode([]byte(`{"Image":"vmfree/shell:demo","ExposedPorts":{"80/tcp":{}},"HostConfig":{"NetworkMode":"host","PortBindings":{"80/tcp":[{"HostIp":"","HostPort":"8080"}],"443/tcp":[{"HostIp":"127.0.0.1","HostPort":"8443"}]}}}`), maxJSON))
	c := readObject(filepath.Join(b.container("web"), "config.json"))
	if got := publishedPorts(c); len(got) != 2 || got[0] != [2]int64{80, 8080} || got[1] != [2]int64{443, 8443} {
		t.Fatalf("saved ports: %v", got)
	}
	item := b.inspect("web")
	binding := item["NetworkSettings"].(obj)["Ports"].(obj)["80/tcp"].([]any)[0].(obj)
	if binding["HostIp"] != "127.0.0.1" || binding["HostPort"] != "8080" {
		t.Fatalf("inspect ports: %v", binding)
	}
	if rows := portRows(item); len(rows) != 2 || rows[0].(obj)["PublicPort"] != int64(8080) || rows[1].(obj)["PrivatePort"] != int64(443) {
		t.Fatalf("list ports: %v", rows)
	}
	for i, bad := range []string{
		`{"80/udp":[{"HostPort":"8080"}]}`,
		`{"80/tcp":[{"HostIp":"0.0.0.0","HostPort":"8080"}]}`,
		`{"80/tcp":[{"HostPort":""}]}`,
		`{"80/tcp":[{"HostPort":"70000"}]}`,
		`{"80/tcp":[{"HostPort":"8080"}],"81/tcp":[{"HostPort":"8080"}]}`,
		`{"80/tcp":[{"HostPort":"8080"},{"HostPort":"8081"}]}`,
	} {
		mustReject(t, func() {
			b.create("bad"+strconv.Itoa(i), decode([]byte(`{"Image":"vmfree/shell:demo","HostConfig":{"PortBindings":`+bad+`}}`), maxJSON))
		})
	}
	// Containers created before port publishing have no saved ports.
	if len(publishedPorts(obj{})) != 0 {
		t.Fatal("legacy config gained ports")
	}
}

func TestScriptEntrypointAndDirectories(t *testing.T) {
	elf := []byte("\x7fELF\x02\x01\x01sh")
	tree := map[string]entry{
		"bin": {Dir: true}, "bin/sh": {Executable: true, Data: elf},
		"usr": {Dir: true}, "usr/bin": {Dir: true}, "usr/bin/env": {Link: "/bin/sh"},
		"entry.sh": {Executable: true, Data: []byte("#!/bin/sh\nexec \"$@\"\n")},
		"with-arg": {Executable: true, Data: []byte("#! /usr/bin/env  sh -x \r\n")},
		"nested":   {Executable: true, Data: []byte("#!/entry.sh\n")},
		"plain":    {Executable: true, Data: []byte("echo no interpreter\n")},
		"var":      {Dir: true}, "var/lib": {Link: "/srv"}, "srv": {Dir: true},
		"file": {Data: []byte("x")},
	}
	im := &image{Tree: tree}
	got := im.command([]string{"/entry.sh", "redis-server"})
	if strings.Join(got, " ") != "/bin/sh /entry.sh redis-server" {
		t.Fatalf("script: %v", got)
	}
	if got = im.command([]string{"/with-arg", "a"}); strings.Join(got, "|") != "/usr/bin/env|sh -x|/with-arg|a" {
		t.Fatalf("interpreter argument: %q", got)
	}
	mustReject(t, func() { im.command([]string{"/nested"}) }) // interpreter is itself a script
	mustReject(t, func() { im.command([]string{"/plain"}) })  // not ELF, no #!
	if im.ensureDir("/data") != "/data" || !tree["data"].Dir {
		t.Fatal("WorkingDir/volume not created")
	}
	im.ensureDir("/var/lib/redis") // through a symlink: lands in /srv
	if !tree["srv/redis"].Dir {
		t.Fatalf("symlinked parent not followed: %v", tree["srv/redis"])
	}
	mustReject(t, func() { im.ensureDir("/file/sub") })
	mustReject(t, func() { im.ensureDir("relative") })
}

func TestNetworkModeAndDNSFiles(t *testing.T) {
	b := testEngine(t)
	for name, mode := range map[string]string{"plain": "", "bridge": `"bridge"`, "host": `"host"`, "none": `"none"`} {
		body := `{"Image":"vmfree/shell:demo"}`
		if mode != "" {
			body = `{"Image":"vmfree/shell:demo","HostConfig":{"NetworkMode":` + mode + `}}`
		}
		b.create(name, decode([]byte(body), maxJSON))
		want := map[bool]string{true: "none", false: "outbound"}[name == "none"]
		if got := readObject(filepath.Join(b.State, name, "config.json"))["network"]; got != want {
			t.Fatalf("%s: network %v", name, got)
		}
	}
	mustReject(t, func() {
		b.create("overlay", decode([]byte(`{"Image":"vmfree/shell:demo","HostConfig":{"NetworkMode":"overlay"}}`), maxJSON))
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
	networkFiles(root, "web")
	if data, _ := os.ReadFile(outside); string(data) != "keep" {
		t.Fatal("symlink followed")
	}
	if data, _ := os.ReadFile(filepath.Join(root, "etc", "hosts")); !strings.Contains(string(data), "127.0.0.1\tweb\n") || !strings.Contains(string(data), "host.docker.internal") {
		t.Fatalf("hosts: %q", data)
	}
	link := t.TempDir()
	check(os.Symlink(t.TempDir(), filepath.Join(link, "etc")))
	mustReject(t, func() { networkFiles(link, "web") }) // a symlinked /etc is refused
}

func TestResourceLimits(t *testing.T) {
	b := testEngine(t)
	b.create("small", decode([]byte(`{"Image":"vmfree/shell:demo","HostConfig":{"Memory":104857601,"MemorySwap":-1,"NanoCpus":1500000000}}`), maxJSON))
	host := b.inspect("small")["HostConfig"].(obj)
	if host["Memory"] != int64(101<<20) || host["NanoCpus"] != int64(2e9) {
		t.Fatalf("limits: %v %v", host["Memory"], host["NanoCpus"])
	}
	// The docker CLI sends zeros when -m and --cpus are absent: the profile's limits apply.
	b.create("plain", decode([]byte(`{"Image":"vmfree/shell:demo","HostConfig":{"Memory":0,"NanoCpus":0,"MemorySwap":0}}`), maxJSON))
	if host := b.inspect("plain")["HostConfig"].(obj); host["Memory"] == int64(0) || host["NanoCpus"] != int64(0) {
		t.Fatalf("defaults: %v %v", host["Memory"], host["NanoCpus"])
	}
	for _, bad := range []string{`{"Memory":1024}`, `{"Memory":70000000000000}`, `{"NanoCpus":65000000000}`, `{"MemorySwap":-2}`} {
		mustReject(t, func() { b.create("bad", decode([]byte(`{"Image":"vmfree/shell:demo","HostConfig":`+bad+`}`), maxJSON)) })
	}
}
