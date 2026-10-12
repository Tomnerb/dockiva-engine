package main

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

var hostPattern = regexp.MustCompile(`^[A-Za-z0-9.-]+(?::[0-9]{1,5})?$`)
var repoPattern = regexp.MustCompile(`^[a-z0-9]+(?:[._-][a-z0-9]+)*(?:/[a-z0-9]+(?:[._-][a-z0-9]+)*)*$`)
var tagPattern = regexp.MustCompile(`^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$`)
var challengeField = regexp.MustCompile(`([a-zA-Z]+)="([^"]*)"`)

func validToken(t string) bool { return t != "" && len(t) <= 16384 && !strings.ContainsAny(t, "\r\n") }

// parseReference applies Docker's naming rules: a first component without "." or ":"
// (and not "localhost") is a Docker Hub repository, and single-name Hub repositories
// live under library/. It returns the registry host, repository, tag or digest, and
// the name the image is cached under (the reference with ":latest" when untagged).
func parseReference(reference string) (host, repo, ref, name string) {
	reject(len(reference) <= 512, "Invalid registry reference")
	host, rest, ok := strings.Cut(reference, "/")
	if !ok || (host != "localhost" && !strings.ContainsAny(host, ".:")) {
		host, rest = "docker.io", reference
	}
	reject(hostPattern.MatchString(host), "Invalid registry host")
	repo, ref, name = rest, "latest", reference+":latest"
	if at := strings.LastIndex(rest, "@"); at >= 0 {
		repo, ref, name = rest[:at], rest[at+1:], reference
	} else if at := strings.LastIndex(rest, ":"); at >= 0 {
		repo, ref, name = rest[:at], rest[at+1:], reference
	}
	reject(repoPattern.MatchString(repo) && (digestPattern.MatchString(ref) || tagPattern.MatchString(ref)), "Invalid registry repository/tag")
	if host == "docker.io" || host == "index.docker.io" {
		// docker.io itself redirects to the marketing site; the registry API lives here.
		host = "registry-1.docker.io"
		if !strings.Contains(repo, "/") {
			repo = "library/" + repo
		}
	}
	return host, repo, ref, name
}

func pullImage(reference, cache string, insecure bool, bearer string) string {
	host, repo, ref, name := parseReference(reference)
	reject(bearer == "" || validToken(bearer), "Invalid registry token")
	reject(!insecure || strings.Split(host, ":")[0] == "127.0.0.1", "Plain HTTP only allowed for literal 127.0.0.1")
	scheme := "https"
	if insecure {
		scheme = "http"
	}
	origin := scheme + "://" + host
	transport := &http.Transport{Proxy: nil}
	defer transport.CloseIdleConnections()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Minute)
	defer cancel()
	// Registries such as Docker Hub serve blobs through CDN redirects. Follow them, but
	// never forward the registry token, and never downgrade from HTTPS. Blob content is
	// still verified against its digest after download.
	client := &http.Client{Transport: transport, Timeout: 10 * time.Minute, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if len(via) >= 5 {
			return fmt.Errorf("too many registry redirects")
		}
		if req.URL.Scheme != "https" && !(insecure && req.URL.Scheme == "http" && req.URL.Hostname() == "127.0.0.1") {
			return fmt.Errorf("registry redirect to %s refused", req.URL.Scheme)
		}
		req.Header.Del("Authorization")
		return nil
	}}
	used := int64(0)
	request := func(address string, limit int64, auth bool) ([]byte, http.Header, int) {
		req, e := http.NewRequestWithContext(ctx, "GET", address, nil)
		check(e)
		req.Header.Set("Accept", manifestMedia+", "+indexMedia)
		req.Header.Set("Accept-Encoding", "identity")
		if bearer != "" && auth {
			req.Header.Set("Authorization", "Bearer "+bearer)
		}
		response, e := client.Do(req)
		check(e)
		defer response.Body.Close()
		data, e := io.ReadAll(io.LimitReader(response.Body, limit+1))
		check(e)
		reject(int64(len(data)) <= limit, "Registry object exceeds limit")
		used += int64(len(data))
		reject(used <= maxPacked, "Pull exceeds 512 MiB budget")
		return data, response.Header, response.StatusCode
	}
	fetch := func(path string, limit int64) []byte {
		data, headers, status := request(origin+path, limit, true)
		if status == 401 && bearer == "" {
			challenge := headers.Get("WWW-Authenticate")
			reject(strings.HasPrefix(strings.ToLower(challenge), "bearer "), "Unsupported registry authentication")
			fields := map[string]string{}
			for _, m := range challengeField.FindAllStringSubmatch(challenge, -1) {
				fields[m[1]] = m[2]
			}
			realm, e := url.Parse(fields["realm"])
			check(e)
			reject(realm.Scheme == "https" && realm.Hostname() != "" && realm.User == nil && realm.Fragment == "", "Invalid HTTPS token realm")
			query := realm.Query()
			query.Set("service", fields["service"])
			query.Set("scope", "repository:"+repo+":pull")
			realm.RawQuery = query.Encode()
			raw, _, code := request(realm.String(), maxJSON, false)
			reject(code == 200, "Token service request failed")
			auth := decode(raw, maxJSON)
			bearer = text(auth["token"])
			if bearer == "" {
				bearer = text(auth["access_token"])
			}
			reject(validToken(bearer), "Invalid registry token")
			data, headers, status = request(origin+path, limit, true)
		}
		reject(status == 200, fmt.Sprintf("Registry request failed with HTTP %d", status))
		if d := headers.Get("Docker-Content-Digest"); d != "" {
			reject(d == digest(data), "Registry digest header mismatch")
		}
		return data
	}
	tmp, e := os.MkdirTemp("", "dockivad-pull-")
	check(e)
	defer os.RemoveAll(tmp)
	check(os.MkdirAll(filepath.Join(tmp, "blobs/sha256"), 0700))
	store := func(data []byte, media string) obj {
		d := digest(data)
		check(os.WriteFile(filepath.Join(tmp, "blobs/sha256", d[7:]), data, 0600))
		return obj{"digest": d, "size": len(data), "mediaType": media}
	}
	data := fetch("/v2/"+repo+"/manifests/"+ref, maxJSON)
	if digestPattern.MatchString(ref) {
		reject(digest(data) == ref, "Requested manifest digest mismatch")
	}
	manifest := decode(data, maxJSON)
	if manifest["mediaType"] == indexMedia {
		items := array(manifest["manifests"])
		reject(len(items) <= 32, "Too many index entries")
		selected := []obj{}
		for _, v := range items {
			d := object(v)
			platform := object(d["platform"])
			if platform["os"] == "linux" && platform["architecture"] == "arm64" {
				selected = append(selected, d)
			}
		}
		reject(len(selected) == 1, "Index must select exactly one linux/arm64 image")
		d := selected[0]
		dg := str(d["digest"])
		reject(digestPattern.MatchString(dg), "Invalid selected digest")
		data = fetch("/v2/"+repo+"/manifests/"+dg, maxJSON)
		reject(digest(data) == dg && int64(len(data)) == integer(d["size"]), "Selected descriptor mismatch")
		manifest = decode(data, maxJSON)
	}
	reject(manifest["mediaType"] == manifestMedia, "Only OCI manifests supported")
	descriptor := store(data, manifestMedia)
	layers := array(manifest["layers"])
	reject(len(layers) <= 32, "Too many layers")
	for _, v := range append([]any{manifest["config"]}, layers...) {
		d := object(v)
		dg := str(d["digest"])
		reject(digestPattern.MatchString(dg), "Invalid blob digest")
		size := integer(d["size"])
		reject(size >= 0 && size <= maxBlob, "Invalid blob size")
		raw := fetch("/v2/"+repo+"/blobs/"+dg, size)
		reject(int64(len(raw)) == size && digest(raw) == dg, "Downloaded blob digest mismatch")
		store(raw, text(d["mediaType"]))
	}
	save(filepath.Join(tmp, "oci-layout"), obj{"imageLayoutVersion": "1.0.0"})
	save(filepath.Join(tmp, "index.json"), obj{"schemaVersion": 2, "manifests": []any{descriptor}})
	return commitImage(cache, name, tmp)
}
