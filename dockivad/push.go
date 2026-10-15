package main

import (
	"bytes"
	"cmp"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"time"
)

// pushImage serves POST /images/{name}/push (docker push, and buildx --push after a
// build): an image's layers, config and manifest go to an OCI Distribution
// registry byte for byte, so a pull of the pushed tag has the same image ID. With
// no tag (docker push --all-tags), every cached tag of the repository goes.
// Progress is Docker's JSON messages; after the stream starts, errors are too.
func (b *engine) pushImage(w http.ResponseWriter, name, tag string, creds credentials) {
	name = shortName(name)
	reject(!strings.Contains(name, "@"), "Push a tag, not a digest")
	if repo := repository(name); tag == "" && repo != name { // NAME:TAG in the path
		name, tag = repo, name[len(repo)+1:]
	}
	names := []string{}
	if tag != "" {
		names = append(names, str(readObject(filepath.Join(b.imagePath(name+":"+tag), "image.json"))["name"]))
	} else {
		entries, e := os.ReadDir(b.Cache)
		check(e)
		for _, entry := range entries {
			meta := filepath.Join(b.Cache, entry.Name(), "image.json")
			if len(entry.Name()) == 64 && exists(meta) && repository(text(readObject(meta)["name"])) == name {
				names = append(names, text(readObject(meta)["name"]))
			}
		}
		sort.Strings(names)
		if len(names) == 0 {
			panic(apiError{404, "An image does not exist locally with the tag: " + name})
		}
	}
	host, repo, _, _ := parseReference(names[0])
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
	say(obj{"status": "The push refers to repository [" + name + "]"})
	r := b.registry(host, repo, creds)
	defer r.close()
	// Cross-repository mounts: other cached repositories of this registry that hold
	// a blob may have it there already (docker tag A B, then docker push B).
	others := map[string]string{} // repository: its image's layout
	entries, e := os.ReadDir(b.Cache)
	check(e)
	for _, entry := range entries {
		meta := filepath.Join(b.Cache, entry.Name(), "image.json")
		if len(entry.Name()) != 64 || !exists(meta) {
			continue
		}
		_ = safeRun(func() {
			if h, other, _, _ := parseReference(text(readObject(meta)["name"])); h == host && other != repo {
				others[other] = filepath.Join(b.Cache, entry.Name(), "layout")
			}
		})
	}
	r.scopes = []string{"repository:" + repo + ":pull,push"}
	for other := range others {
		r.scopes = append(r.scopes, "repository:"+other+":pull")
	}
	sort.Strings(r.scopes[1:])
	r.send("GET", r.origin+"/v2/", nil, "", 200) // authenticates once, for every scope
	for _, n := range names {
		im := loadImage(noLinks(b.imagePath(n), "layout"))
		for _, ld := range im.layers {
			say(obj{"status": "Preparing", "progressDetail": obj{}, "id": str(ld["digest"])[7:19]})
		}
		for _, ld := range im.layers[1:] {
			say(obj{"status": "Waiting", "progressDetail": obj{}, "id": str(ld["digest"])[7:19]})
		}
		pushed := map[string]bool{}
		for i, d := range im.blobs[1:] { // the config, then each layer
			if pushed[d] {
				continue
			}
			pushed[d] = true
			from := ""
			for _, other := range r.scopes[1:] {
				if other = strings.TrimSuffix(strings.TrimPrefix(other, "repository:"), ":pull"); exists(filepath.Join(others[other], "blobs/sha256", d[7:])) {
					from = other
					break
				}
			}
			status := r.blob(noLinks(im.layout, "blobs/sha256/"+d[7:]), d, from, func(current, total int64) {
				if i > 0 {
					say(obj{"status": "Pushing", "progressDetail": obj{"current": current, "total": total}, "id": d[7:19]})
				}
			})
			if i > 0 {
				say(obj{"status": status, "progressDetail": obj{}, "id": d[7:19]})
			}
		}
		manifest := blob(im.layout, im.descriptor, maxJSON, manifestMedia, dockerManifest)
		t := n[len(repository(n))+1:]
		response := r.send("PUT", r.origin+"/v2/"+repo+"/manifests/"+t, bodyOf(manifest), text(im.descriptor["mediaType"]), 201)
		response.Body.Close()
		reject(response.Header.Get("Docker-Content-Digest") == "" || response.Header.Get("Docker-Content-Digest") == im.Digest, "Registry changed the manifest digest")
		say(obj{"status": fmt.Sprintf("%s: digest: %s size: %d", t, im.Digest, len(manifest)), "progressDetail": obj{}})
		say(obj{"progressDetail": obj{}, "aux": obj{"Tag": t, "Digest": im.Digest, "Size": len(manifest)}})
	}
}

// distributionInspect serves GET /distribution/{name}/json, which buildx --push
// reads after the push: the registry's manifest descriptor, and its platforms.
func (b *engine) distributionInspect(name string, creds credentials) obj {
	host, repo, ref, _ := parseReference(name)
	r := b.registry(host, repo, creds)
	defer r.close()
	r.scopes = []string{"repository:" + repo + ":pull"}
	response := r.send("GET", r.origin+"/v2/"+repo+"/manifests/"+ref, nil, "", 200)
	defer response.Body.Close()
	raw, e := io.ReadAll(io.LimitReader(response.Body, maxJSON+1))
	check(e)
	manifest := decode(raw, maxJSON)
	media, _, _ := strings.Cut(response.Header.Get("Content-Type"), ";")
	platforms := []any{}
	if items, ok := manifest["manifests"].([]any); ok { // an index
		for _, v := range items {
			if p, ok := object(v)["platform"].(map[string]any); ok {
				platforms = append(platforms, p)
			}
		}
	} else {
		config := r.send("GET", r.origin+"/v2/"+repo+"/blobs/"+blobDigest(object(manifest["config"]), maxJSON, configMedia, dockerConfig), nil, "", 200)
		defer config.Body.Close()
		cr, e := io.ReadAll(io.LimitReader(config.Body, maxJSON+1))
		check(e)
		c := decode(cr, maxJSON)
		platforms = append(platforms, obj{"architecture": c["architecture"], "os": c["os"]})
	}
	return obj{"Descriptor": obj{"mediaType": cmp.Or(text(manifest["mediaType"]), media), "digest": digest(raw), "size": len(raw)}, "Platforms": platforms}
}

// registry is a session with one repository of a Distribution registry. Plain
// HTTP reaches it only for a loopback host with --insecure-local.
type registry struct {
	ctx           context.Context
	cancel        func()
	origin, repo  string
	plain         bool
	client        *http.Client
	creds         credentials
	scopes        []string // for the token: set before the first request
	authorization string
}

func (b *engine) registry(host, repo string, creds credentials) *registry {
	plain := b.Insecure && loopback(host)
	scheme := "https"
	if plain {
		scheme = "http"
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Hour)
	r := &registry{ctx: ctx, cancel: cancel, origin: scheme + "://" + host, repo: repo, plain: plain, creds: creds, client: registryClient(plain)}
	if creds.RegistryToken != "" {
		r.authorization = "Bearer " + creds.RegistryToken
	}
	return r
}
func (r *registry) close() { r.cancel(); r.client.CloseIdleConnections() }

func bodyOf(data []byte) func() (io.ReadCloser, int64) {
	return func() (io.ReadCloser, int64) { return io.NopCloser(bytes.NewReader(data)), int64(len(data)) }
}

// send makes a request, answering a 401 challenge once, and panics unless the
// response has one of the expected statuses. A Location goes to the registry's
// own host only, with the registry's credentials.
func (r *registry) send(method, address string, body func() (io.ReadCloser, int64), media string, expect ...int) *http.Response {
	u, e := url.Parse(address)
	check(e)
	reject(allowedURL(u, r.plain), "Registry address refused: "+u.Scheme+"://"+u.Host)
	for attempt := 0; ; attempt++ {
		req, e := http.NewRequestWithContext(r.ctx, method, address, nil)
		check(e)
		if body != nil {
			req.Body, req.ContentLength = body()
			req.Header.Set("Content-Type", media)
		}
		req.Header.Set("Accept", strings.Join([]string{manifestMedia, indexMedia, dockerManifest, dockerList}, ", "))
		if r.authorization != "" && r.origin == u.Scheme+"://"+u.Host {
			req.Header.Set("Authorization", r.authorization)
		}
		response, e := r.client.Do(req)
		if e != nil {
			panic(apiError{502, "Registry request failed: " + e.Error()})
		}
		if response.StatusCode == 401 && attempt == 0 && r.creds.RegistryToken == "" {
			challenge := response.Header.Get("WWW-Authenticate")
			response.Body.Close()
			r.authorization = authorize(r.ctx, r.client, challenge, r.scopes, r.creds, r.plain)
			continue
		}
		for _, status := range expect {
			if response.StatusCode == status {
				return response
			}
		}
		defer response.Body.Close()
		panic(apiError{502, registryError(response)})
	}
}

// blob makes sure the repository has a blob: already there, mounted from another
// repository of the registry, or uploaded (POST, PATCH with progress, PUT). It
// returns Docker's status line for the layer.
func (r *registry) blob(path, d, from string, progress func(current, total int64)) string {
	blobs := r.origin + "/v2/" + r.repo + "/blobs/"
	response := r.send("HEAD", blobs+d, nil, "", 200, 404)
	response.Body.Close()
	if response.StatusCode == 200 {
		return "Layer already exists"
	}
	start := blobs + "uploads/"
	if from != "" {
		start += "?" + url.Values{"mount": {d}, "from": {from}}.Encode()
	}
	response = r.send("POST", start, nil, "", 201, 202)
	response.Body.Close()
	if response.StatusCode == 201 {
		return "Mounted from " + from
	}
	upload := r.location(response)
	fd, e := syscall.Open(path, syscall.O_RDONLY|syscall.O_NOFOLLOW, 0)
	check(e)
	f := os.NewFile(uintptr(fd), d)
	defer f.Close()
	st, e := f.Stat()
	check(e)
	size := st.Size()
	response = r.send("PATCH", upload, func() (io.ReadCloser, int64) {
		_, e := f.Seek(0, io.SeekStart)
		check(e)
		return &uploading{f: f, total: size, progress: progress}, size
	}, "application/octet-stream", 202, 204)
	response.Body.Close()
	upload = r.location(response)
	u, e := url.Parse(upload)
	check(e)
	q := u.Query()
	q.Set("digest", d)
	u.RawQuery = q.Encode()
	response = r.send("PUT", u.String(), bodyOf(nil), "application/octet-stream", 201, 204)
	response.Body.Close()
	return "Pushed"
}

// location is an upload response's Location, resolved against its request.
func (r *registry) location(response *http.Response) string {
	loc, e := url.Parse(response.Header.Get("Location"))
	reject(e == nil && response.Header.Get("Location") != "", "Registry upload without a Location")
	return response.Request.URL.ResolveReference(loc).String()
}

// uploading reads a blob for upload, reporting progress about ten times a second.
// Not an embedded *os.File: its WriteTo would bypass Read.
type uploading struct {
	f        *os.File
	n, total int64
	last     time.Time
	progress func(current, total int64)
}

func (u *uploading) Read(b []byte) (int, error) {
	n, e := u.f.Read(b)
	u.n += int64(n)
	if time.Since(u.last) > 100*time.Millisecond || e == io.EOF {
		u.last = time.Now()
		u.progress(u.n, u.total)
	}
	return n, e
}
func (u *uploading) Close() error { return nil } // blob closes the file
