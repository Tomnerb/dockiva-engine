package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// tarOf is a build context: name to content (a trailing "/" makes a directory).
func tarOf(t *testing.T, files map[string]string) *bytes.Buffer {
	t.Helper()
	var buf bytes.Buffer
	w := tar.NewWriter(&buf)
	for name, content := range files {
		h := &tar.Header{Name: name, Mode: 0o644, Size: int64(len(content)), Typeflag: tar.TypeReg}
		if strings.HasSuffix(name, "/") {
			h = &tar.Header{Name: name, Mode: 0o755, Typeflag: tar.TypeDir}
		}
		check(w.WriteHeader(h))
		if h.Typeflag == tar.TypeReg {
			_, e := w.Write([]byte(content))
			check(e)
		}
	}
	check(w.Close())
	return &buf
}

// buildMessages posts a build and returns its JSON messages.
func buildMessages(t *testing.T, b *engine, query string, body *bytes.Buffer) []obj {
	t.Helper()
	r := request(t, b.socketHandler(), "POST", "/v1.45/build?"+query, body.String(), "")
	if r.Code != 200 {
		t.Fatalf("build: %d %s", r.Code, r.Body.String())
	}
	out := []obj{}
	for _, line := range strings.Split(strings.TrimSpace(r.Body.String()), "\n") {
		var m obj
		check(json.Unmarshal([]byte(line), &m))
		out = append(out, m)
	}
	return out
}

func buildResult(t *testing.T, messages []obj) (id, stream, failure string) {
	for _, m := range messages {
		if s, ok := m["stream"].(string); ok {
			stream += s
		}
		if e, ok := m["error"].(string); ok {
			failure = e
		}
		if aux, ok := m["aux"].(map[string]any); ok {
			id, _ = aux["ID"].(string)
		}
	}
	return
}

func TestBuildCopyAddMetadataAndStages(t *testing.T) {
	b := testEngine(t)
	var archive bytes.Buffer
	gz := gzip.NewWriter(&archive)
	inner := tarOf(t, map[string]string{"inside.txt": "from the archive"})
	_, e := gz.Write(inner.Bytes())
	check(e)
	check(gz.Close())
	dockerfile := `# a comment
ARG BASE=dockiva/shell:demo
FROM ${BASE} AS base
ARG BASE
ENV GREETING="hello world" \
    ORIGIN=$BASE
WORKDIR /srv
COPY app/ ./app/
COPY *.json /etc/conf/
ADD data.tar.gz /opt/data/
LABEL org.test=yes
EXPOSE 8080 53/udp
USER 1000
HEALTHCHECK --interval=5s --retries=2 CMD ["true"]
CMD ["sh", "-c", "echo $GREETING"]

FROM scratch AS final
COPY --from=base /srv/app/a.txt /a.txt
ENV ONLY=final
`
	context := func() *bytes.Buffer {
		return tarOf(t, map[string]string{"Dockerfile": dockerfile, "app/": "", "app/a.txt": "a", "app/sub/": "", "app/sub/b.txt": "b",
			"config.json": "{}", "data.tar.gz": archive.String()})
	}

	id, stream, failure := buildResult(t, buildMessages(t, b, "t=test/app:1&target=base", context()))
	if failure != "" || !strings.HasPrefix(id, "sha256:") || !strings.Contains(stream, "Step 2/16 : FROM ${BASE} AS base") {
		t.Fatalf("target build: id %q failure %q\n%s", id, failure, stream)
	}
	im := b.image("test/app:1")
	if im.Digest != id || im.WorkingDir != "/srv" || im.User != "1000" || strings.Join(im.Args, " ") != "sh -c echo $GREETING" {
		t.Fatalf("settings: %+v", im)
	}
	env := strings.Join(im.Env, "|")
	if !strings.Contains(env, "GREETING=hello world") || !strings.Contains(env, "ORIGIN=dockiva/shell:demo") {
		t.Fatalf("env: %v", im.Env)
	}
	if integer(im.Healthcheck["Interval"]) != 5e9 || integer(im.Healthcheck["Retries"]) != 2 {
		t.Fatalf("healthcheck: %v", im.Healthcheck)
	}
	_, config := imageDocs(im.layout)
	settings := object(config["config"])
	if object(settings["Labels"])["org.test"] != "yes" || object(settings["ExposedPorts"])["53/udp"] == nil {
		t.Fatalf("labels and ports: %v", settings)
	}
	for name, want := range map[string]string{"srv/app/a.txt": "a", "srv/app/sub/b.txt": "b", "etc/conf/config.json": "{}", "opt/data/inside.txt": "from the archive"} {
		if got, e := os.ReadFile(filepath.Join(im.Root, name)); e != nil || string(got) != want {
			t.Fatalf("%s: %q %v", name, got, e)
		}
	}
	base := b.image("dockiva/shell:demo")
	if len(im.layers) != len(base.layers)+1 {
		t.Fatalf("layers: %d on %d", len(im.layers), len(base.layers))
	}

	// The last stage, untagged: found by its ID, as docker run IMAGE-ID does.
	id2, _, failure := buildResult(t, buildMessages(t, b, "", context()))
	if failure != "" || id2 == id {
		t.Fatalf("final build: %q %q", id2, failure)
	}
	final := b.image(id2[7:19])
	entries, e := os.ReadDir(final.Root)
	check(e)
	if len(entries) != 1 || entries[0].Name() != "a.txt" || len(final.layers) != 1 || strings.Join(final.Env, "|") != "ONLY=final" {
		t.Fatalf("final stage: %v %d %v", entries, len(final.layers), final.Env)
	}

	// A rebuild moves the tag; a Dockerfile alone (docker build - < Dockerfile) is a build too.
	id3, _, failure := buildResult(t, buildMessages(t, b, "t=test/app:1", bytes.NewBufferString("FROM dockiva/shell:demo\nENV REBUILT=1\n")))
	if failure != "" || b.image("test/app:1").Digest != id3 || id3 == id {
		t.Fatalf("rebuild: %q %q", id3, failure)
	}

	// Errors arrive as a JSON message after the progress so far.
	for dockerfile, want := range map[string]string{
		"FROM dockiva/shell:demo\nRUN --mount=type=cache,target=/x true\n": "needs BuildKit",
		"FROM dockiva/shell:demo\nCOPY missing.txt /\n":                    "missing.txt not found",
		"ENV A=1\n":                        "comes before FROM",
		"FROM dockiva/shell:demo\nBOGUS\n": "Unknown instruction: BOGUS",
	} {
		_, _, failure := buildResult(t, buildMessages(t, b, "t=bad", tarOf(t, map[string]string{"Dockerfile": dockerfile})))
		if !strings.Contains(failure, want) {
			t.Fatalf("%q: error %q, want %q", dockerfile, failure, want)
		}
	}
	if exists(filepath.Join(b.Cache, key("bad:latest"))) {
		t.Fatal("a failed build was tagged")
	}
}

func TestImageFilter(t *testing.T) {
	for _, c := range []struct {
		name, filters string
		want          bool
	}{
		{"test/app:1", `{"reference":{"test/app":true}}`, true},
		{"test/app:1", `{"reference":{"test/app:1":true}}`, true},
		{"test/app:1", `{"reference":{"test/*":true}}`, true},
		{"test/app:1", `{"reference":{"other":true}}`, false},
		{"localhost:5000/app:2", `{"reference":{"localhost:5000/app":true}}`, true},
		{"<none>:0123456789ab", `{"dangling":{"true":true}}`, true},
		{"test/app:1", `{"dangling":{"true":true}}`, false},
	} {
		if got := imageFilter(c.name, decode([]byte(c.filters), maxJSON)); got != c.want {
			t.Fatalf("%s %s: %v", c.name, c.filters, got)
		}
	}
}
