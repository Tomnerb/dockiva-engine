package main

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
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
	return host, repo, ref, shortName(name)
}

// shortName is Docker's familiar form of a Docker Hub name (nginx:alpine for
// docker.io/library/nginx:alpine), so a name finds its image however it was pulled.
func shortName(name string) string {
	for _, prefix := range []string{"docker.io/library/", "index.docker.io/library/", "docker.io/", "index.docker.io/"} {
		if rest, ok := strings.CutPrefix(name, prefix); ok {
			return rest
		}
	}
	return name
}

// credentials are docker login's, as the docker CLI sends them in X-Registry-Auth.
type credentials struct {
	Username      string `json:"username"`
	Password      string `json:"password"`
	IdentityToken string `json:"identitytoken"`
	RegistryToken string `json:"registrytoken"`
}

// registryCredentials decodes X-Registry-Auth (base64url JSON; "" when absent).
func registryCredentials(header string) credentials {
	var c credentials
	if header == "" {
		return c
	}
	raw, e := base64.URLEncoding.DecodeString(header)
	if e != nil {
		raw, e = base64.StdEncoding.DecodeString(header)
	}
	reject(e == nil && json.Unmarshal(raw, &c) == nil, "Invalid X-Registry-Auth header")
	reject(c.RegistryToken == "" || validToken(c.RegistryToken), "Invalid registry token")
	return c
}

// loopback is a registry host --insecure-local lets plain HTTP reach.
func loopback(host string) bool {
	if h, _, e := net.SplitHostPort(host); e == nil {
		host = h
	}
	return host == "127.0.0.1" || host == "localhost"
}

// allowedURL: HTTPS, or plain HTTP to a loopback host when plain is allowed.
func allowedURL(u *url.URL, plain bool) bool {
	return u.Scheme == "https" || plain && u.Scheme == "http" && loopback(u.Host)
}

// registryClient follows redirects (registries serve blobs through CDNs) without
// the registry's Authorization, and never from HTTPS to plain HTTP.
func registryClient(plain bool) *http.Client {
	return &http.Client{Transport: &http.Transport{Proxy: nil, ResponseHeaderTimeout: time.Minute}, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if len(via) >= 5 {
			return fmt.Errorf("too many registry redirects")
		}
		if !allowedURL(req.URL, plain) {
			return fmt.Errorf("registry redirect to %s refused", req.URL.Scheme)
		}
		req.Header.Del("Authorization")
		return nil
	}}
}

// registryError is a registry's error response as Docker words it ("denied:
// requested access to the resource is denied"), else its HTTP status.
func registryError(response *http.Response) string {
	raw, _ := io.ReadAll(io.LimitReader(response.Body, 1<<16))
	var body struct {
		Errors  []struct{ Code, Message string }
		Details string
	}
	if json.Unmarshal(raw, &body) == nil {
		if len(body.Errors) > 0 {
			e := body.Errors[0]
			return strings.ToLower(strings.ReplaceAll(e.Code, "_", " ")) + ": " + e.Message
		}
		if body.Details != "" {
			return "unauthorized: " + body.Details
		}
	}
	return fmt.Sprintf("registry %s %s: HTTP %d", response.Request.Method, response.Request.URL.Path, response.StatusCode)
}

// authorize answers a registry's 401 challenge (WWW-Authenticate) and returns the
// Authorization value: Basic with the credentials, or a Bearer token for scopes
// from the challenge's token service (OAuth2 refresh with an identity token, else
// a GET with the credentials as Basic, else anonymous).
func authorize(ctx context.Context, client *http.Client, challenge string, scopes []string, c credentials, plain bool) string {
	scheme, _, _ := strings.Cut(challenge, " ")
	switch strings.ToLower(scheme) {
	case "basic":
		reject(c.Username != "", "unauthorized: authentication required")
		return "Basic " + base64.StdEncoding.EncodeToString([]byte(c.Username+":"+c.Password))
	case "bearer":
	default:
		panic(apiError{502, "Unsupported registry authentication"})
	}
	fields := map[string]string{}
	for _, m := range challengeField.FindAllStringSubmatch(challenge, -1) {
		fields[m[1]] = m[2]
	}
	realm, e := url.Parse(fields["realm"])
	check(e)
	reject(allowedURL(realm, plain) && realm.Hostname() != "" && realm.User == nil && realm.Fragment == "", "Invalid HTTPS token realm")
	var req *http.Request
	if c.IdentityToken != "" {
		form := url.Values{"grant_type": {"refresh_token"}, "refresh_token": {c.IdentityToken}, "service": {fields["service"]}, "scope": {strings.Join(scopes, " ")}, "client_id": {"dockiva"}}
		req, e = http.NewRequestWithContext(ctx, "POST", realm.String(), strings.NewReader(form.Encode()))
		check(e)
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	} else {
		query := realm.Query()
		query.Set("service", fields["service"])
		query["scope"] = scopes
		realm.RawQuery = query.Encode()
		req, e = http.NewRequestWithContext(ctx, "GET", realm.String(), nil)
		check(e)
		if c.Username != "" {
			req.SetBasicAuth(c.Username, c.Password)
		}
	}
	response, e := client.Do(req)
	if e != nil {
		panic(apiError{502, "Token service request failed: " + e.Error()})
	}
	defer response.Body.Close()
	if response.StatusCode != 200 {
		panic(apiError{502, registryError(response)})
	}
	raw, e := io.ReadAll(io.LimitReader(response.Body, maxJSON+1))
	check(e)
	auth := decode(raw, maxJSON)
	token := text(auth["token"])
	if token == "" {
		token = text(auth["access_token"])
	}
	reject(validToken(token), "Invalid registry token")
	return "Bearer " + token
}

func pullImage(reference, cache string, insecure bool, bearer string) string {
	return pullWith(reference, cache, insecure, credentials{RegistryToken: bearer})
}

// pullWith pulls with docker login's credentials. insecure (--insecure-local) lets
// a loopback registry (127.0.0.1, localhost) use plain HTTP; others are HTTPS.
func pullWith(reference, cache string, insecure bool, creds credentials) string {
	host, repo, ref, name := parseReference(reference)
	reject(creds.RegistryToken == "" || validToken(creds.RegistryToken), "Invalid registry token")
	plain := insecure && loopback(host)
	scheme := "https"
	if plain {
		scheme = "http"
	}
	origin := scheme + "://" + host
	client := registryClient(plain)
	defer client.CloseIdleConnections()
	// A large image over a slow link may take long; a transfer that stalls fails sooner.
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Hour)
	defer cancel()
	// Blob content from a CDN redirect is still verified against its digest.
	authorization := ""
	if creds.RegistryToken != "" {
		authorization = "Bearer " + creds.RegistryToken
	}
	used := int64(0)
	// request GETs address; to, when given, receives the body (streamed, up to limit).
	request := func(address string, limit int64, to io.Writer) ([]byte, http.Header, int) {
		// Each request ends if no bytes arrive for two minutes.
		reqCtx, stop := context.WithCancel(ctx)
		defer stop()
		stall := time.AfterFunc(2*time.Minute, stop)
		defer stall.Stop()
		req, e := http.NewRequestWithContext(reqCtx, "GET", address, nil)
		check(e)
		req.Header.Set("Accept", strings.Join([]string{manifestMedia, indexMedia, dockerManifest, dockerList}, ", "))
		req.Header.Set("Accept-Encoding", "identity")
		if authorization != "" {
			req.Header.Set("Authorization", authorization)
		}
		response, e := client.Do(req)
		if e != nil {
			panic(apiError{502, "Registry request failed: " + e.Error()})
		}
		defer response.Body.Close()
		if to != nil && response.StatusCode == 200 {
			n, e := io.Copy(to, io.LimitReader(&progress{response.Body, stall}, limit+1))
			if e != nil {
				panic(apiError{502, "Registry download failed: " + e.Error()})
			}
			reject(n <= limit, "Registry object exceeds limit")
			used += n
			reject(used <= maxBlob, "Pull exceeds 64 GiB")
			return nil, response.Header, response.StatusCode
		}
		if response.StatusCode != 200 && response.StatusCode != 401 {
			panic(apiError{502, registryError(response)})
		}
		data, e := io.ReadAll(io.LimitReader(response.Body, min(limit, maxJSON)+1))
		check(e)
		reject(int64(len(data)) <= min(limit, maxJSON), "Registry object exceeds limit")
		used += int64(len(data))
		return data, response.Header, response.StatusCode
	}
	fetch := func(path string, limit int64, to io.Writer) []byte {
		data, headers, status := request(origin+path, limit, to)
		if status == 401 && creds.RegistryToken == "" {
			authorization = authorize(ctx, client, headers.Get("WWW-Authenticate"), []string{"repository:" + repo + ":pull"}, creds, plain)
			data, headers, status = request(origin+path, limit, to)
		}
		reject(status == 200, fmt.Sprintf("Registry request failed with HTTP %d", status))
		if d := headers.Get("Docker-Content-Digest"); d != "" && to == nil {
			reject(d == digest(data), "Registry digest header mismatch")
		}
		return data
	}
	// In the cache, so committing links the blobs instead of copying them.
	tmp, e := os.MkdirTemp(private(cache), ".pull-")
	check(e)
	defer os.RemoveAll(tmp)
	check(os.MkdirAll(filepath.Join(tmp, "blobs/sha256"), 0700))
	store := func(data []byte, media string) obj {
		d := digest(data)
		check(os.WriteFile(filepath.Join(tmp, "blobs/sha256", d[7:]), data, 0600))
		return obj{"digest": d, "size": len(data), "mediaType": media}
	}
	data := fetch("/v2/"+repo+"/manifests/"+ref, maxJSON, nil)
	if digestPattern.MatchString(ref) {
		reject(digest(data) == ref, "Requested manifest digest mismatch")
	}
	manifest := decode(data, maxJSON)
	if manifest["mediaType"] == indexMedia || manifest["mediaType"] == dockerList {
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
		data = fetch("/v2/"+repo+"/manifests/"+dg, maxJSON, nil)
		reject(digest(data) == dg && int64(len(data)) == integer(d["size"]), "Selected descriptor mismatch")
		manifest = decode(data, maxJSON)
	}
	media := text(manifest["mediaType"])
	if media == "" {
		media = manifestMedia // an OCI manifest may omit its type
	}
	reject(media == manifestMedia || media == dockerManifest, "Unsupported manifest type: "+media)
	descriptor := store(data, media)
	layers := array(manifest["layers"])
	reject(len(layers) <= 128, "Too many layers")
	for _, v := range append([]any{manifest["config"]}, layers...) {
		d := object(v)
		dg := str(d["digest"])
		reject(digestPattern.MatchString(dg), "Invalid blob digest")
		size := integer(d["size"])
		reject(size >= 0 && size <= maxBlob, "Invalid blob size")
		// Stream to disk, hashing on the way (a blob listed twice, such as the empty layer, once).
		p := filepath.Join(tmp, "blobs/sha256", dg[7:])
		if exists(p) {
			continue
		}
		f, e := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
		check(e)
		hash := sha256.New()
		fetch("/v2/"+repo+"/blobs/"+dg, size, io.MultiWriter(f, hash))
		st, e := f.Stat()
		check(errors.Join(e, f.Close()))
		reject(st.Size() == size && "sha256:"+hex.EncodeToString(hash.Sum(nil)) == dg, "Downloaded blob digest mismatch")
	}
	save(filepath.Join(tmp, "oci-layout"), obj{"imageLayoutVersion": "1.0.0"})
	save(filepath.Join(tmp, "index.json"), obj{"schemaVersion": 2, "manifests": []any{descriptor}})
	return commitImage(cache, name, tmp)
}

// progress pushes back a stall timer on every read that returns data.
type progress struct {
	r     io.Reader
	stall *time.Timer
}

func (p *progress) Read(b []byte) (int, error) {
	n, e := p.r.Read(b)
	if n > 0 {
		p.stall.Reset(2 * time.Minute)
	}
	return n, e
}
