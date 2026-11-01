package main

import (
	"encoding/base64"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"
)

// testRegistry is a minimal OCI Distribution registry with token auth: blob HEAD,
// uploads (POST, PATCH, PUT), cross-repository mounts, manifests, and GETs.
type testRegistry struct {
	sync.Mutex
	blobs     map[string]map[string][]byte // repository: digest: content
	manifests map[string][]byte            // repository:reference
	uploads   map[string][]byte
	scopes    []string // scopes of granted tokens
	mounts    int
}

var registryPath = regexp.MustCompile(`^/v2/(.+)/(blobs|manifests)/(.+)$`)

func (g *testRegistry) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	g.Lock()
	defer g.Unlock()
	if r.URL.Path == "/token" {
		if user, pass, _ := r.BasicAuth(); user != "alice" || pass != "secret" {
			w.WriteHeader(401)
			_, _ = io.WriteString(w, `{"details":"incorrect username or password"}`)
			return
		}
		g.scopes = append(g.scopes, r.URL.Query()["scope"]...)
		_, _ = io.WriteString(w, `{"token":"granted"}`)
		return
	}
	if r.Header.Get("Authorization") != "Bearer granted" {
		w.Header().Set("WWW-Authenticate", `Bearer realm="http://`+r.Host+`/token",service="test"`)
		w.WriteHeader(401)
		_, _ = io.WriteString(w, `{"errors":[{"code":"UNAUTHORIZED","message":"authentication required"}]}`)
		return
	}
	if r.URL.Path == "/v2/" {
		return
	}
	if strings.HasPrefix(r.URL.Path, "/upload/") {
		id := strings.TrimPrefix(r.URL.Path, "/upload/")
		data, e := io.ReadAll(r.Body)
		check(e)
		g.uploads[id] = append(g.uploads[id], data...)
		if r.Method == "PATCH" {
			w.Header().Set("Location", r.URL.RequestURI())
			w.WriteHeader(202)
			return
		}
		repo, d := r.URL.Query().Get("repo"), r.URL.Query().Get("digest")
		if digest(g.uploads[id]) != d {
			w.WriteHeader(400)
			_, _ = io.WriteString(w, `{"errors":[{"code":"DIGEST_INVALID","message":"provided digest did not match uploaded content"}]}`)
			return
		}
		g.blobs[repo][d] = g.uploads[id]
		w.WriteHeader(201)
		return
	}
	upload := regexp.MustCompile(`^/v2/(.+)/blobs/uploads/$`).FindStringSubmatch(r.URL.Path)
	if upload != nil && r.Method == "POST" {
		repo := upload[1]
		if g.blobs[repo] == nil {
			g.blobs[repo] = map[string][]byte{}
		}
		q := r.URL.Query()
		if data, ok := g.blobs[q.Get("from")][q.Get("mount")]; ok {
			g.blobs[repo][q.Get("mount")] = data
			g.mounts++
			w.WriteHeader(201)
			return
		}
		id := token()[:8]
		w.Header().Set("Location", "/upload/"+id+"?repo="+repo) // relative, as registry:2 may send
		w.WriteHeader(202)
		return
	}
	m := registryPath.FindStringSubmatch(r.URL.Path)
	if m == nil {
		w.WriteHeader(404)
		return
	}
	repo, kind, ref := m[1], m[2], m[3]
	var data []byte
	var ok bool
	if kind == "manifests" && r.Method == "PUT" {
		body, e := io.ReadAll(r.Body)
		check(e)
		g.manifests[repo+":"+ref], g.manifests[repo+":"+digest(body)] = body, body
		w.Header().Set("Docker-Content-Digest", digest(body))
		w.WriteHeader(201)
		return
	}
	if kind == "manifests" {
		data, ok = g.manifests[repo+":"+ref]
	} else {
		data, ok = g.blobs[repo][ref]
	}
	if !ok {
		w.WriteHeader(404)
		return
	}
	if r.Method == "GET" {
		_, _ = w.Write(data)
	}
}

func pushMessages(t *testing.T, b *engine, path string, auth any) []obj {
	t.Helper()
	r := httptest.NewRequest("POST", path, nil)
	raw, _ := json.Marshal(auth)
	r.Header.Set("X-Registry-Auth", base64.URLEncoding.EncodeToString(raw))
	w := httptest.NewRecorder()
	b.socketHandler().ServeHTTP(w, r)
	if w.Code != 200 {
		t.Fatalf("push %s: %d %s", path, w.Code, w.Body.String())
	}
	messages := []obj{}
	dec := json.NewDecoder(w.Body)
	for dec.More() {
		m := obj{}
		check(dec.Decode(&m))
		messages = append(messages, m)
	}
	return messages
}

func statuses(messages []obj) string {
	out := []string{}
	for _, m := range messages {
		out = append(out, text(m["status"])+text(m["error"]))
	}
	return strings.Join(out, "\n")
}

func TestPushToRegistry(t *testing.T) {
	g := &testRegistry{blobs: map[string]map[string][]byte{}, manifests: map[string][]byte{}, uploads: map[string][]byte{}}
	server := httptest.NewServer(g)
	defer server.Close()
	host := strings.TrimPrefix(server.URL, "http://")
	b := testEngine(t)
	b.Insecure = true
	source := noLinks(b.imagePath("dockiva/shell:demo"), "layout")
	want := loadImage(source).Digest
	commitImage(b.Cache, host+"/team/app:1", source)
	alice := obj{"username": "alice", "password": "secret", "serveraddress": host}

	// Wrong password: the token service's reason, as a JSON error message.
	out := statuses(pushMessages(t, b, "/v1.45/images/"+host+"/team/app/push?tag=1", obj{"username": "alice", "password": "wrong"}))
	if !strings.Contains(out, "unauthorized: incorrect username or password") {
		t.Fatalf("auth failure:\n%s", out)
	}

	// A new upload: every layer is pushed, then the manifest, with its digest unchanged.
	messages := pushMessages(t, b, "/v1.45/images/"+host+"/team/app/push?tag=1", alice)
	out = statuses(messages)
	if !strings.Contains(out, "Pushed") || strings.Contains(out, "error") || !strings.Contains(out, "1: digest: "+want+" size: ") {
		t.Fatalf("push:\n%s", out)
	}
	if aux := object(messages[len(messages)-1]["aux"]); aux["Tag"] != "1" || aux["Digest"] != want {
		t.Fatalf("aux: %v", messages[len(messages)-1])
	}
	if digest(g.manifests["team/app:1"]) != want || len(g.scopes) == 0 || g.scopes[0] != "repository:team/app:pull,push" {
		t.Fatalf("manifest or scopes: %v", g.scopes)
	}

	// buildx --push then reads the pushed digest back (GET /distribution/NAME/json).
	get := httptest.NewRequest("GET", "/v1.45/distribution/"+host+"/team/app:1/json", nil)
	raw, _ := json.Marshal(alice)
	get.Header.Set("X-Registry-Auth", base64.URLEncoding.EncodeToString(raw))
	inspect := httptest.NewRecorder()
	b.socketHandler().ServeHTTP(inspect, get)
	if !strings.Contains(inspect.Body.String(), want) || !strings.Contains(inspect.Body.String(), `"architecture":"arm64"`) {
		t.Fatalf("distribution inspect: %d %s", inspect.Code, inspect.Body.String())
	}
	// Pushed again: the registry already has each layer.
	if out := statuses(pushMessages(t, b, "/v1.45/images/"+host+"/team/app/push?tag=1", alice)); strings.Contains(out, "Pushed") || !strings.Contains(out, "Layer already exists") {
		t.Fatalf("repush:\n%s", out)
	}

	// docker tag + push to another repository: layers mount from the first one.
	commitImage(b.Cache, host+"/other/app:2", source)
	out = statuses(pushMessages(t, b, "/v1.45/images/"+host+"/other/app/push?tag=2", alice))
	if !strings.Contains(out, "Mounted from team/app") || strings.Contains(out, "Pushed") || g.mounts == 0 {
		t.Fatalf("mount:\n%s", out)
	}
	if !strings.Contains(strings.Join(g.scopes, " "), "repository:team/app:pull") {
		t.Fatalf("mount scope: %v", g.scopes)
	}

	// --all-tags: no tag pushes every cached tag of the repository.
	commitImage(b.Cache, host+"/team/app:3", source)
	out = statuses(pushMessages(t, b, "/v1.45/images/"+host+"/team/app/push", alice))
	if !strings.Contains(out, "1: digest: "+want) || !strings.Contains(out, "3: digest: "+want) {
		t.Fatalf("all tags:\n%s", out)
	}

	// Pulled back (with the same credentials), the image keeps its ID.
	cache := filepath.Join(t.TempDir(), "cache")
	if got := loadImage(pullWith(host+"/other/app:2", cache, true, credentials{Username: "alice", Password: "secret"}, func(name, layout string) string { return commitImage(cache, name, layout) })).Digest; got != want {
		t.Fatalf("pulled %s, pushed %s", got, want)
	}

	// Without --insecure-local, a plain-HTTP registry is refused; a missing tag is a 404.
	b.Insecure = false
	if out := statuses(pushMessages(t, b, "/v1.45/images/"+host+"/team/app/push?tag=1", alice)); !strings.Contains(out, "HTTP response to HTTPS client") {
		t.Fatalf("plain HTTP without --insecure-local:\n%s", out)
	}
	r := httptest.NewRequest("POST", "/images/"+host+"/team/app/push?tag=missing", nil)
	w := httptest.NewRecorder()
	b.socketHandler().ServeHTTP(w, r)
	if w.Code != 404 {
		t.Fatalf("missing tag: %d %s", w.Code, w.Body.String())
	}
}

func TestRegistryCredentials(t *testing.T) {
	raw := base64.URLEncoding.EncodeToString([]byte(`{"username":"u","password":"p","identitytoken":"i"}`))
	if c := registryCredentials(raw); c.Username != "u" || c.Password != "p" || c.IdentityToken != "i" {
		t.Fatalf("decoded %+v", c)
	}
	if c := registryCredentials(""); c != (credentials{}) {
		t.Fatal("empty header")
	}
	mustReject(t, func() { registryCredentials("!!") })
	if loopback("example.com:5000") || !loopback("127.0.0.1:5000") || !loopback("localhost") {
		t.Fatal("loopback")
	}
}

func TestInspectRegistryNameIsNoSuchContainer(t *testing.T) {
	b := testEngine(t)
	r := request(t, b.socketHandler(), "GET", "/v1.45/containers/127.0.0.1:5055/x:1/json", "", "")
	if r.Code != 404 || !strings.Contains(r.Body.String(), "No such container") {
		t.Fatalf("docker inspect of a host:port image name: %d %s", r.Code, r.Body.String())
	}
}
