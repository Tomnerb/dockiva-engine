package main

import (
	"bufio"
	"context"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"reflect"
	"slices"
	"sort"
	"strings"
	"testing"
	"time"
)

func TestProtobufAndDockerignore(t *testing.T) {
	when := time.Unix(1700000000, 42)
	m := pbDecode(pbuf{}.str(1, "ref").num(2, 300).flag(3, true).time(4, when).strMap(5, map[string]string{"a": "1", "b": ""}).msg(6, pbuf{}.str(1, "inner")))
	got := pbDecode(m.last(4).b)
	if m.str(1) != "ref" || m.num(2) != 300 || m.num(3) != 1 || got.num(1) != 1700000000 || got.num(2) != 42 ||
		!reflect.DeepEqual(m.strMap(5), map[string]string{"a": "1", "b": ""}) || m.sub(6).str(1) != "inner" {
		t.Fatalf("protobuf round trip: %v", m)
	}
	if safeRun(func() { pbDecode([]byte{0x0a, 5, 'x'}) }) == nil {
		t.Fatal("truncated field accepted")
	}
	want := []string{"node_modules", "*.log", "!keep.log", "build", "a/b"}
	if got := dockerignorePatterns([]byte("\ufeff# comment\n node_modules \n\n*.log\n! keep.log\n/build/\n./a//b\n")); !reflect.DeepEqual(got, want) {
		t.Fatalf("dockerignore: %q", got)
	}
}

// dialUpgrade opens one of the engine's h2c endpoints as the docker client does.
func dialUpgrade(t *testing.T, addr, target string, header http.Header) *h2cConn {
	t.Helper()
	conn, e := net.Dial("tcp", addr)
	check(e)
	t.Cleanup(func() { conn.Close() })
	req, e := http.NewRequest("POST", "http://engine"+target, nil)
	check(e)
	if header != nil {
		req.Header = header
	}
	req.Header.Set("Connection", "Upgrade")
	req.Header.Set("Upgrade", "h2c")
	check(req.Write(conn))
	br := bufio.NewReader(conn)
	resp, e := http.ReadResponse(br, req)
	check(e)
	if resp.StatusCode != 101 {
		t.Fatalf("%s: HTTP %d", target, resp.StatusCode)
	}
	return &h2cConn{Conn: conn, r: br, done: make(chan struct{})}
}

// fileSyncServer is a client session's FileSync (as buildx serves it): files are
// "local/path" to content; include and exclude patterns match whole paths.
func fileSyncServer(files map[string]string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/grpc")
		w.WriteHeader(200)
		if r.URL.Path == "/moby.buildkit.secrets.v1.Secrets/GetSecret" { // buildx --secret: "secret/ID" in files
			m, _ := grpcRead(r.Body)
			value, ok := files["secret/"+pbDecode(m).str(1)]
			if !ok {
				w.Header().Set(http.TrailerPrefix+"Grpc-Status", "5")
				w.Header().Set(http.TrailerPrefix+"Grpc-Message", "rpc error: code = NotFound desc = not found")
				return
			}
			grpcWrite(w, pbuf{}.str(1, value))
			w.Header().Set(http.TrailerPrefix+"Grpc-Status", "0")
			return
		}
		defer w.Header().Set(http.TrailerPrefix+"Grpc-Status", "0")
		if r.URL.Path == "/grpc.health.v1.Health/Check" {
			grpcRead(r.Body)
			grpcWrite(w, pbuf{}.num(1, 1))
			return
		}
		patterns := func(k string) []string {
			out := []string{}
			for _, v := range r.Header.Values(k) {
				v, _ = url.QueryUnescape(v)
				out = append(out, v)
			}
			return out
		}
		include, exclude := patterns("Include-Patterns"), patterns("Exclude-Patterns")
		entries := map[string]bool{} // path: is a directory
		for name := range files {
			local, rel, _ := strings.Cut(name, "/")
			if local != r.Header.Get("Dir-Name") || slices.Contains(exclude, rel) || len(include) > 0 && !slices.Contains(include, rel) {
				continue
			}
			entries[rel] = false
			for d := path.Dir(rel); d != "."; d = path.Dir(d) {
				entries[d] = true
			}
		}
		names := []string{}
		for n := range entries {
			names = append(names, n)
		}
		sort.Strings(names)
		for _, n := range names {
			mode := uint64(0o644)
			if entries[n] {
				mode = uint64(os.ModeDir | 0o755)
			}
			grpcWrite(w, pbuf{}.msg(2, pbuf{}.str(1, n).num(2, mode).num(6, uint64(time.Now().UnixNano()))))
		}
		grpcWrite(w, pbuf{}) // an empty stat ends the listing
		for {
			m, e := grpcRead(r.Body)
			if e != nil {
				return
			}
			p := pbDecode(m)
			switch p.num(1) {
			case packetReq:
				id := p.num(3)
				grpcWrite(w, pbuf{}.num(1, packetData).num(3, id).bytes(4, []byte(files[r.Header.Get("Dir-Name")+"/"+names[id]])))
				grpcWrite(w, pbuf{}.num(1, packetData).num(3, id))
			case packetFin:
				grpcWrite(w, pbuf{}.num(1, packetFin))
				return
			}
		}
	}
}

// TestBuildKitDockerfileBuild drives a build as buildx's docker driver does: a
// session, Control.Solve, the gateway's Solve and Return, and Status.
func TestBuildKitDockerfileBuild(t *testing.T) {
	b := testEngine(t)
	srv := httptest.NewServer(b.socketHandler())
	defer srv.Close()
	addr := srv.Listener.Addr().String()
	files := map[string]string{
		"dockerfile/Dockerfile": "FROM dockiva/shell:demo\nARG V\nENV X=$V\nCOPY . /files/\n",
		"dockerfile/Bad":        "FROM dockiva/shell:demo\nRUN --mount=type=secret,id=missing,required true\n",
		"dockerfile/Secret": "FROM dockiva/shell:demo\nRUN --mount=type=secret,id=tok,env=TOK --mount=type=secret,id=tok,target=/s,mode=0444 " +
			"busybox sh -c 'busybox echo \"$TOK\" > /got; busybox cat /s >> /got'\n",
		"secret/tok":            "s3cret",
		"context/.dockerignore": "# not this\nignored.txt\n",
		"context/a.txt":         "A",
		"context/sub/b.txt":     "B",
		"context/ignored.txt":   "secret",
	}
	sid, ref := "s-"+token()[:8], "r-"+token()[:8] // engine-wide names
	session := dialUpgrade(t, addr, "/session", http.Header{"X-Docker-Expose-Session-Uuid": {sid}})
	go serveH2(session, fileSyncServer(files))
	rt := h2Client(dialUpgrade(t, addr, "/grpc", nil))
	ctx := context.Background()
	if w := grpcUnary(ctx, rt, bkControl+"ListWorkers", nil, nil).sub(1); w.sub(3).str(1) != "arm64" || w.sub(2).str(1) == "org.mobyproject.buildkit.worker.snapshotter" {
		t.Fatalf("worker: %v", w)
	}

	build := func(ref, dockerfile string) (solve pbFields, gateway, solveErr error, vertexes []string) {
		solved := make(chan error, 1)
		go func() {
			solved <- safeRun(func() {
				solve = grpcUnary(ctx, rt, bkControl+"Solve", nil, pbuf{}.str(1, ref).str(5, sid).
					msg(13, pbuf{}.str(1, "moby").strMap(2, map[string]string{"name": "docker.io/library/bk-unit:latest,bk-unit:two"})))
			})
		}()
		md := http.Header{"Buildkit-Controlapi-Buildid": {ref}}
		if grpcUnary(ctx, rt, bkGateway+"Ping", md, nil)[1] == nil {
			t.Fatal("no gateway capabilities")
		}
		ret := pbuf{}
		gateway = safeRun(func() {
			res := grpcUnary(ctx, rt, bkGateway+"Solve", md, pbuf{}.str(2, "dockerfile.v0").
				strMap(3, map[string]string{"filename": dockerfile, "build-arg:V": "value", "no-cache": ""}).flag(5, true))
			if !strings.HasPrefix(res.sub(3).sub(3).str(1), "sha256:") {
				t.Fatalf("gateway result: %v", res)
			}
		})
		if gateway != nil {
			ret = ret.msg(2, pbuf{}.num(1, 2).str(2, gateway.Error()))
		}
		grpcUnary(ctx, rt, bkGateway+"Return", md, ret)
		solveErr = <-solved
		status := grpcOpen(ctx, rt, bkControl+"Status", nil)
		defer status.close()
		check(status.send(pbuf{}.str(1, ref)))
		status.closeSend()
		for m, ok := status.recv(); ok; m, ok = status.recv() {
			for _, v := range m[1] {
				if vx := pbDecode(v.b); len(vx[6]) > 0 { // completed
					vertexes = append(vertexes, vx.str(3)+" "+vx.str(7))
				}
			}
		}
		return
	}

	solve, gateway, solveErr, vertexes := build(ref, "Dockerfile")
	if gateway != nil || solveErr != nil {
		t.Fatalf("build: %v %v", gateway, solveErr)
	}
	want := []string{"[internal] load build definition from Dockerfile ", "[internal] load .dockerignore ", "[internal] load build context ",
		"[1/4] FROM dockiva/shell:demo ", "[2/4] ARG V ", "[3/4] ENV X=$V ", "[4/4] COPY . /files/ ", "exporting to image "}
	if !reflect.DeepEqual(vertexes, want) {
		t.Fatalf("progress: %q", vertexes)
	}
	response := solve.strMap(1)
	im := b.image("bk-unit:two")
	if response["containerimage.digest"] != im.Digest || b.image("bk-unit:latest").Digest != im.Digest {
		t.Fatalf("exported %v, tagged %s", response, im.Digest)
	}
	if string(readBounded(im.Root, "files/sub/b.txt", 10)) != "B" || !exists(filepath.Join(im.Root, "files/a.txt")) || exists(filepath.Join(im.Root, "files/ignored.txt")) {
		t.Fatal("context files")
	}
	if _, config := imageDocs(im.layout); !slices.Contains(stringsOfAny(object(config["config"])["Env"]), "X=value") {
		t.Fatalf("build arg: %v", config["config"])
	}

	_, gateway, solveErr, vertexes = build(ref+"-bad", "Bad")
	missing := "RUN --mount=type=secret,id=missing: no such secret; pass it with docker build --secret id=missing,src=FILE"
	if gateway == nil || !strings.Contains(gateway.Error(), missing) || solveErr == nil || !strings.Contains(solveErr.Error(), missing) ||
		!strings.Contains(vertexes[len(vertexes)-1], "[2/2] RUN --mount=type=secret,id=missing,required true "+missing) {
		t.Fatalf("missing secret: %v / %v / %q", gateway, solveErr, vertexes)
	}

	// A secret from the session: a file and a variable, outside the image.
	if !exists("../build/dockivad") {
		t.Skip("RUN needs ../build/dockivad (scripts/bundle_dockiva_engine.py)")
	}
	b.Self, b.Worker = "../build/dockivad", "../build/dockiva-worker"
	if _, gateway, solveErr, _ = build(ref+"-secret", "Secret"); gateway != nil || solveErr != nil {
		t.Fatalf("secret build: %v %v", gateway, solveErr)
	}
	root := b.image("bk-unit:two").Root
	if got := string(readBounded(root, "got", 100)); got != "s3cret\ns3cret" || exists(filepath.Join(root, "s")) {
		t.Fatalf("secret: %q", got)
	}
}
