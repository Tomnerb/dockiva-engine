package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// buildx's read of a URL context: an HTTP source copied (unpacked) into an empty
// tree, then StatFile and ReadFile on the result.
func TestLLBHTTPContext(t *testing.T) {
	var tgz bytes.Buffer
	gz := gzip.NewWriter(&tgz)
	tw := tar.NewWriter(gz)
	for name, body := range map[string]string{"Dockerfile": "FROM alpine\n", "Dockerfile.rego": "package docker\n"} {
		tw.WriteHeader(&tar.Header{Name: name, Mode: 0o644, Size: int64(len(body)), Typeflag: tar.TypeReg})
		tw.Write([]byte(body))
	}
	tw.Close()
	gz.Close()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Write(tgz.Bytes()) }))
	defer srv.Close()

	source := pbuf{}.msg(3, pbuf{}.str(1, srv.URL+"/ctx.tar.gz").strMap(2, map[string]string{"http.filename": "context"}))
	copyAction := pbuf{}.num(1, ^uint64(0)).num(2, 0).num(3, 0).msg(4, pbuf{}.str(1, "/context").str(2, "/").num(7, 1)) // input -1 (scratch), copy with unpack
	file := pbuf{}.msg(1, pbuf{}.str(1, digest(source))).msg(4, pbuf{}.msg(2, copyAction))
	terminal := pbuf{}.msg(1, pbuf{}.str(1, digest(file)))
	def := pbuf{}.bytes(1, source).bytes(1, file).bytes(1, terminal)

	bd := &builder{ctx: context.Background(), work: t.TempDir(), say: func(string) {}}
	root := bd.llbEval(pbDecode(def))
	if got, e := os.ReadFile(filepath.Join(root, "Dockerfile.rego")); e != nil || string(got) != "package docker\n" {
		t.Fatalf("unpacked tree: %q %v", got, e)
	}
	st := pbDecode(llbStat(root, "Dockerfile.rego")).sub(1)
	if st.str(1) != "Dockerfile.rego" || st.num(5) != uint64(len("package docker\n")) {
		t.Fatalf("stat: %v", st)
	}
	if data := pbDecode(llbRead(root, "/Dockerfile.rego", 8, 6)).last(1).b; string(data) != "docker" {
		t.Fatalf("ranged read: %q", data)
	}
	if e := safeRun(func() { llbStat(root, "missing.rego") }); e == nil || !strings.Contains(e.Error(), "no such file") {
		t.Fatalf("missing file: %v", e) // buildx reads "no such file" as no policy
	}
	if e := safeRun(func() {
		bd.llbEval(pbDecode(pbuf{}.bytes(1, pbuf{}.msg(2, pbuf{})).bytes(1, pbuf{}.msg(1, pbuf{}.str(1, digest(pbuf{}.msg(2, pbuf{})))))))
	}); e == nil {
		t.Fatal("an exec op was accepted")
	}
}
