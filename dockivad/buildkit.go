package main

// docker build through buildx (Docker CLI 23+): the part of BuildKit's API that
// buildx's "docker" driver uses, served by the classic builder (build.go).
//
//	POST /grpc     a hijacked h2c connection serving BuildKit's Control service
//	               (ListWorkers, Info, Solve, Status) and the gateway (LLBBridge Ping,
//	               Solve, Return) that buildx runs its Dockerfile build through.
//	POST /session  a hijacked h2c connection to the client's session; here the engine
//	               is the gRPC client and fetches the Dockerfile and context with
//	               FileSync.DiffCopy (buildkit_build.go).
//
// buildx picks its "docker" driver when POST /grpc upgrades; otherwise it starts a
// moby/buildkit container (the docker-container driver), which the engine cannot run.
// gRPC here is net/http's unencrypted HTTP/2 with length-prefixed protobuf
// messages, encoded by hand: only the fields buildx sends and reads.

import (
	"bufio"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// pbuf builds a protobuf message.
type pbuf []byte

func (b pbuf) bytes(n int, v []byte) pbuf {
	b = binary.AppendUvarint(b, uint64(n)<<3|2)
	return append(binary.AppendUvarint(b, uint64(len(v))), v...)
}
func (b pbuf) msg(n int, m pbuf) pbuf { return b.bytes(n, m) }
func (b pbuf) str(n int, v string) pbuf {
	if v == "" {
		return b
	}
	return b.bytes(n, []byte(v))
}
func (b pbuf) num(n int, v uint64) pbuf {
	if v == 0 {
		return b
	}
	return binary.AppendUvarint(binary.AppendUvarint(b, uint64(n)<<3), v)
}
func (b pbuf) flag(n int, v bool) pbuf {
	if !v {
		return b
	}
	return b.num(n, 1)
}
func (b pbuf) time(n int, t time.Time) pbuf { // google.protobuf.Timestamp
	if t.IsZero() {
		return b
	}
	return b.msg(n, pbuf{}.num(1, uint64(t.Unix())).num(2, uint64(t.Nanosecond())))
}
func (b pbuf) strMap(n int, m map[string]string) pbuf {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		b = b.msg(n, pbuf{}.str(1, k).str(2, m[k]))
	}
	return b
}

// pbFields is a decoded protobuf message: each field's values in order (varints
// as numbers, length-delimited fields as bytes; fixed-size fields are skipped).
type pbFields map[int][]pbValue
type pbValue struct {
	n uint64
	b []byte
}

func pbDecode(b []byte) pbFields {
	f := pbFields{}
	for len(b) > 0 {
		k, n := binary.Uvarint(b)
		reject(n > 0, "Malformed protobuf message")
		b = b[n:]
		var v pbValue
		switch k & 7 {
		case 0:
			v.n, n = binary.Uvarint(b)
			reject(n > 0, "Malformed protobuf message")
			b = b[n:]
		case 1, 5:
			size := 8
			if k&7 == 5 {
				size = 4
			}
			reject(len(b) >= size, "Malformed protobuf message")
			b = b[size:]
		case 2:
			l, n := binary.Uvarint(b)
			reject(n > 0 && l <= uint64(len(b)-n), "Malformed protobuf message")
			v.b, b = b[n:n+int(l)], b[n+int(l):]
		default:
			reject(false, "Malformed protobuf message")
		}
		f[int(k>>3)] = append(f[int(k>>3)], v)
	}
	return f
}
func (f pbFields) last(n int) pbValue {
	if v := f[n]; len(v) > 0 {
		return v[len(v)-1]
	}
	return pbValue{}
}
func (f pbFields) str(n int) string   { return string(f.last(n).b) }
func (f pbFields) num(n int) uint64   { return f.last(n).n }
func (f pbFields) sub(n int) pbFields { return pbDecode(f.last(n).b) }
func (f pbFields) strMap(n int) map[string]string {
	m := map[string]string{}
	for _, e := range f[n] {
		kv := pbDecode(e.b)
		m[kv.str(1)] = kv.str(2)
	}
	return m
}

// grpcError is a gRPC status: a code (2 unknown, 12 unimplemented) and a message.
type grpcError struct {
	code    int
	message string
}

func (e grpcError) Error() string { return e.message }

func grpcFrame(m []byte) []byte {
	out := make([]byte, 5, 5+len(m))
	binary.BigEndian.PutUint32(out[1:], uint32(len(m)))
	return append(out, m...)
}

// grpcRead reads one message; io.EOF at the end of the stream.
func grpcRead(r io.Reader) ([]byte, error) {
	var h [5]byte
	if _, e := io.ReadFull(r, h[:]); e != nil {
		return nil, e
	}
	reject(h[0] == 0, "Compressed gRPC messages are not supported")
	n := binary.BigEndian.Uint32(h[1:])
	reject(n <= 16<<20, "gRPC message exceeds 16 MiB")
	m := make([]byte, n)
	_, e := io.ReadFull(r, m)
	return m, e
}

func grpcRequest(r *http.Request) pbFields {
	m, e := grpcRead(r.Body)
	reject(e == nil, "Expected a gRPC request message")
	return pbDecode(m)
}

func grpcWrite(w http.ResponseWriter, m []byte) {
	_, e := w.Write(grpcFrame(m))
	check(e)
	check(http.NewResponseController(w).Flush())
}

// grpcEscape is grpc-message's percent-encoding.
func grpcEscape(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if c := s[i]; c < 0x20 || c > 0x7e || c == '%' {
			fmt.Fprintf(&b, "%%%02X", c)
		} else {
			b.WriteByte(c)
		}
	}
	return b.String()
}

// h2cConn is a connection hijacked for HTTP/2 after its 101 response: reads go
// through the HTTP server's buffered reader, and done closes with the connection.
type h2cConn struct {
	net.Conn
	r    *bufio.Reader
	once sync.Once
	done chan struct{}
}

func (c *h2cConn) Read(p []byte) (int, error) { return c.r.Read(p) }
func (c *h2cConn) Close() error {
	c.once.Do(func() { close(c.done) })
	return c.Conn.Close()
}

func hijackH2C(w http.ResponseWriter, r *http.Request) *h2cConn {
	reject(strings.EqualFold(r.Header.Get("Upgrade"), "h2c"), "This endpoint needs Upgrade: h2c")
	hj, ok := w.(http.Hijacker)
	reject(ok, "This endpoint needs a hijackable connection")
	conn, rw, e := hj.Hijack()
	check(e)
	_ = conn.SetDeadline(time.Time{})
	c := &h2cConn{Conn: conn, r: rw.Reader, done: make(chan struct{})}
	if _, e := io.WriteString(conn, "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: h2c\r\n\r\n"); e != nil {
		c.Close()
	}
	return c
}

func h2Protocols() *http.Protocols {
	var p http.Protocols
	p.SetUnencryptedHTTP2(true)
	return &p
}

// oneConn is a listener for a single connection: http.Server.Serve on it returns
// once that connection closes.
type oneConn struct {
	c     *h2cConn
	taken atomic.Bool
}

func (l *oneConn) Accept() (net.Conn, error) {
	if !l.taken.Swap(true) {
		return l.c, nil
	}
	<-l.c.done
	return nil, net.ErrClosed
}
func (l *oneConn) Close() error   { return nil }
func (l *oneConn) Addr() net.Addr { return l.c.LocalAddr() }

func serveH2(c *h2cConn, h http.Handler) {
	_ = (&http.Server{Handler: h, Protocols: h2Protocols()}).Serve(&oneConn{c: c})
}

// h2Client is an HTTP/2 client over one connection (prior knowledge, no TLS).
func h2Client(c net.Conn) *http.Transport {
	var used atomic.Bool
	return &http.Transport{Protocols: h2Protocols(), DisableCompression: true, DialContext: func(context.Context, string, string) (net.Conn, error) {
		if used.Swap(true) {
			return nil, errors.New("the build session has ended")
		}
		return c, nil
	}}
}

// grpcCall is a client gRPC stream. The response (its headers) arrives in the
// background: a server may wait for the request's first message before answering.
type grpcCall struct {
	method string
	pw     *io.PipeWriter
	cancel context.CancelFunc
	done   chan struct{}
	resp   *http.Response
	err    error
}

func grpcOpen(ctx context.Context, rt http.RoundTripper, method string, md http.Header) *grpcCall {
	ctx, cancel := context.WithCancel(ctx)
	pr, pw := io.Pipe()
	req, e := http.NewRequestWithContext(ctx, "POST", "http://buildkit"+method, pr)
	check(e)
	if md != nil {
		req.Header = md.Clone()
	}
	req.Header.Set("Content-Type", "application/grpc")
	req.Header.Set("Te", "trailers")
	c := &grpcCall{method: method, pw: pw, cancel: cancel, done: make(chan struct{})}
	go func() {
		c.resp, c.err = rt.RoundTrip(req)
		close(c.done)
	}()
	return c
}
func (c *grpcCall) send(m pbuf) error { _, e := c.pw.Write(grpcFrame(m)); return e }
func (c *grpcCall) closeSend()        { c.pw.Close() }
func (c *grpcCall) close() {
	c.pw.CloseWithError(context.Canceled)
	c.cancel()
	<-c.done
	if c.resp != nil {
		c.resp.Body.Close()
	}
}
func (c *grpcCall) response() *http.Response {
	<-c.done
	if c.err != nil {
		panic(apiError{502, "The build session failed: " + c.err.Error()})
	}
	reject(c.resp.StatusCode == 200, fmt.Sprintf("The build session answered %s: HTTP %d", c.method, c.resp.StatusCode))
	return c.resp
}

// recv is the next message; false at the end of a stream that ended OK.
func (c *grpcCall) recv() (pbFields, bool) {
	resp := c.response()
	m, e := grpcRead(resp.Body)
	if e == io.EOF {
		code, message := resp.Trailer.Get("Grpc-Status"), resp.Trailer.Get("Grpc-Message")
		if code == "" { // a trailers-only response
			code, message = resp.Header.Get("Grpc-Status"), resp.Header.Get("Grpc-Message")
		}
		if code != "0" {
			message, _ = url.PathUnescape(message)
			panic(apiError{502, "The build session failed: " + message})
		}
		return nil, false
	}
	if e != nil {
		panic(apiError{502, "The build session failed: " + e.Error()})
	}
	return pbDecode(m), true
}

func grpcUnary(ctx context.Context, rt http.RoundTripper, method string, md http.Header, req pbuf) pbFields {
	c := grpcOpen(ctx, rt, method, md)
	defer c.close()
	check(c.send(req))
	c.closeSend()
	m, ok := c.recv()
	reject(ok, "Empty gRPC response to "+method)
	for _, more := c.recv(); more; _, more = c.recv() {
	}
	return m
}

// bkSession is a client's session (POST /session) the engine calls into.
type bkSession struct{ rt *http.Transport }

var bkSessions = struct {
	sync.Mutex
	m map[string]*bkSession
}{m: map[string]*bkSession{}}

// bkSessionFor waits for a session to connect: buildx dials it alongside the solve.
// ponytail: polls every 20 ms; a condition variable if sessions ever get busy.
func bkSessionFor(ctx context.Context, id string) *bkSession {
	for deadline := time.Now().Add(30 * time.Second); ; {
		bkSessions.Lock()
		s := bkSessions.m[id]
		bkSessions.Unlock()
		if s != nil {
			return s
		}
		reject(time.Now().Before(deadline), "The build session "+id+" did not connect")
		select {
		case <-ctx.Done():
			panic(apiError{499, "Build cancelled"})
		case <-time.After(20 * time.Millisecond):
		}
	}
}

// buildkitConn serves POST /grpc and POST /session; it returns when the client
// closes the connection.
func (b *engine) buildkitConn(w http.ResponseWriter, r *http.Request, path string) {
	if path == "/grpc" {
		serveH2(hijackH2C(w, r), http.HandlerFunc(b.buildkitCall))
		return
	}
	id := r.Header.Get("X-Docker-Expose-Session-Uuid")
	reject(id != "", "A session needs X-Docker-Expose-Session-Uuid")
	bkSessions.Lock()
	_, taken := bkSessions.m[id]
	bkSessions.Unlock()
	reject(!taken, "Session "+id+" already exists")
	c := hijackH2C(w, r)
	bkServeSession(id, c, c.done)
}

// bkServeSession keeps a client's session until it closes (done): conn carries
// HTTP/2 to the client's gRPC services (FileSync, Auth, ...).
func bkServeSession(id string, conn net.Conn, done <-chan struct{}) {
	defer conn.Close()
	s := &bkSession{rt: h2Client(conn)}
	defer s.rt.CloseIdleConnections()
	// A health check opens the HTTP/2 connection, whose reader then notices when the
	// client hangs up (Docker's session manager checks health too).
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if safeRun(func() { grpcUnary(ctx, s.rt, "/grpc.health.v1.Health/Check", nil, nil) }) != nil {
		return // a session buildx closes at once, as after reading capabilities
	}
	bkSessions.Lock()
	if bkSessions.m[id] != nil {
		bkSessions.Unlock()
		return
	}
	bkSessions.m[id] = s
	bkSessions.Unlock()
	defer func() {
		bkSessions.Lock()
		delete(bkSessions.m, id)
		bkSessions.Unlock()
	}()
	<-done
}

// grpcConn is a byte stream tunneled through a bidirectional gRPC call of
// BytesMessages (Control.Session, which buildx uses for sessions it shares
// between builds).
type grpcConn struct {
	r    io.Reader
	w    http.ResponseWriter
	rest []byte
	mu   sync.Mutex
	once sync.Once
	done chan struct{}
}

func (c *grpcConn) Read(p []byte) (int, error) {
	for len(c.rest) == 0 {
		var m []byte
		var e error
		if err := safeRun(func() { m, e = grpcRead(c.r) }); err != nil {
			return 0, err
		}
		if e != nil {
			return 0, e
		}
		if e = safeRun(func() { c.rest = pbDecode(m).last(1).b }); e != nil {
			return 0, e
		}
	}
	n := copy(p, c.rest)
	c.rest = c.rest[n:]
	return n, nil
}
func (c *grpcConn) Write(p []byte) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	select {
	case <-c.done: // the call has ended
		return 0, net.ErrClosed
	default:
	}
	if _, e := c.w.Write(grpcFrame(pbuf{}.bytes(1, p))); e != nil {
		return 0, e
	}
	return len(p), http.NewResponseController(c.w).Flush()
}
func (c *grpcConn) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.once.Do(func() { close(c.done) })
	return nil
}
func (c *grpcConn) LocalAddr() net.Addr                { return &net.UnixAddr{Name: "grpc", Net: "unix"} }
func (c *grpcConn) RemoteAddr() net.Addr               { return &net.UnixAddr{Name: "grpc", Net: "unix"} }
func (c *grpcConn) SetDeadline(t time.Time) error      { return nil }
func (c *grpcConn) SetReadDeadline(t time.Time) error  { return nil }
func (c *grpcConn) SetWriteDeadline(t time.Time) error { return nil }

// buildkitCall serves one gRPC call on a /grpc connection.
func (b *engine) buildkitCall(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/grpc")
	w.WriteHeader(200)
	code, message := 0, ""
	if e := safeRun(func() { b.buildkitMethod(w, r) }); e != nil {
		code, message = 2, e.Error()
		var ge grpcError
		if errors.As(e, &ge) {
			code = ge.code
		}
	}
	w.Header().Set(http.TrailerPrefix+"Grpc-Status", strconv.Itoa(code))
	if message != "" {
		w.Header().Set(http.TrailerPrefix+"Grpc-Message", grpcEscape(message))
	}
}

// bkUsage is a step cache entry as a BuildKit UsageRecord.
func bkUsage(e obj) pbuf {
	at := func(k string) time.Time { t, _ := time.Parse(time.RFC3339Nano, text(e[k])); return t }
	return pbuf{}.str(1, str(e["ID"])).num(4, uint64(integer(e["Size"]))).time(6, at("CreatedAt")).time(7, at("LastUsedAt")).
		num(8, uint64(integer(e["UsageCount"]))).str(9, text(e["Description"])).str(10, text(e["Type"]))
}

const bkControl, bkGateway = "/moby.buildkit.v1.Control/", "/moby.buildkit.v1.frontend.LLBBridge/"

func (b *engine) buildkitMethod(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	switch r.URL.Path {
	case bkControl + "ListWorkers":
		grpcRequest(r)
		grpcWrite(w, pbuf{}.msg(1, bkWorker()))
	case bkControl + "Info":
		grpcRequest(r)
		grpcWrite(w, pbuf{}.msg(1, bkVersion()))
	case bkControl + "DiskUsage": // the step cache, as docker buildx du lists it
		grpcRequest(r)
		buildCacheMu.Lock()
		entries := b.buildCacheEntries()
		buildCacheMu.Unlock()
		out := pbuf{}
		for _, e := range entries {
			out = out.msg(1, bkUsage(object(e)))
		}
		grpcWrite(w, out)
	case bkControl + "Prune": // docker builder prune: one UsageRecord per removed entry
		q := grpcRequest(r)
		age, keep := time.Duration(q.num(3)), int64(max(q.num(4), q.num(5))) // keepDuration; reservedSpace, maxUsedSpace
		buildCacheMu.Lock()
		entries := b.buildCacheEntries()
		deleted, _ := b.pruneBuildCache(keep, func(used time.Time) bool { return age == 0 || time.Since(used) > age })
		buildCacheMu.Unlock()
		for _, e := range entries {
			if slices.Contains(deleted, str(object(e)["ID"])) {
				grpcWrite(w, bkUsage(object(e)))
			}
		}
	case bkControl + "Solve":
		grpcWrite(w, b.bkControlSolve(ctx, grpcRequest(r)))
	case bkControl + "Session":
		id := r.Header.Get("X-Docker-Expose-Session-Uuid")
		reject(id != "", "A session needs x-docker-expose-session-uuid")
		c := &grpcConn{r: r.Body, w: w, done: make(chan struct{})}
		bkServeSession(id, c, c.done)
	case bkControl + "Status":
		bkSolveFor(grpcRequest(r).str(1)).progress.stream(ctx, w)
	case bkGateway + "Ping":
		grpcRequest(r)
		caps := pbuf{}
		for _, id := range []string{"solve.base", "return", "returnmap", "proto.refarray", "readfile", "statfile"} {
			caps = caps.msg(1, pbuf{}.str(1, id).flag(2, true))
		}
		for _, id := range llbCaps {
			caps = caps.msg(2, pbuf{}.str(1, id).flag(2, true))
		}
		grpcWrite(w, caps.msg(3, bkWorker()))
	case bkGateway + "Solve":
		grpcWrite(w, b.bkGatewaySolve(ctx, r.Header.Get("Buildkit-Controlapi-Buildid"), grpcRequest(r)))
	case bkGateway + "StatFile": // StatFileRequest{Ref, path}
		q := grpcRequest(r)
		grpcWrite(w, llbStat(bkSolveFor(r.Header.Get("Buildkit-Controlapi-Buildid")).llbRef(q.str(1)), q.str(2)))
	case bkGateway + "ReadFile": // ReadFileRequest{Ref, FilePath, Range{Offset, Length}}
		q := grpcRequest(r)
		rng := q.sub(3)
		grpcWrite(w, llbRead(bkSolveFor(r.Header.Get("Buildkit-Controlapi-Buildid")).llbRef(q.str(1)), q.str(2), int64(rng.num(1)), int64(rng.num(2))))
	case bkGateway + "Return":
		bkReturn(r.Header.Get("Buildkit-Controlapi-Buildid"), grpcRequest(r))
		grpcWrite(w, nil)
	default:
		panic(grpcError{12, "Dockiva Engine does not implement BuildKit's " + r.URL.Path})
	}
}

func bkVersion() pbuf {
	return pbuf{}.str(1, "github.com/moby/buildkit").str(2, "dockiva-engine").str(3, "classic-builder")
}

// bkWorker has no snapshotter label: buildx then treats the engine as Docker's
// classic image store (no multi-platform, OCI export or attestations).
func bkWorker() pbuf {
	return pbuf{}.str(1, "dockiva").strMap(2, map[string]string{"org.mobyproject.buildkit.worker.executor": "dockiva-engine"}).
		msg(3, pbuf{}.str(1, "arm64").str(2, "linux")).msg(5, bkVersion())
}

// bkSolve is one build (a Control.Solve "Ref"): buildx sends Control.Solve, then
// drives the build through the gateway (Solve, then Return) and reads Status.
type bkSolve struct {
	progress   *bkProgress
	ready      chan struct{} // Control.Solve arrived: session and exports below are set
	readyOnce  sync.Once
	returned   chan struct{} // the gateway's Return arrived
	returnOnce sync.Once
	session    string
	names      []string   // image names to export
	outputs    []bkOutput // files exports (--output type=local or tar)
	mu         sync.Mutex
	failure    string  // Return's error
	id         string  // the image built
	llb        llbRefs // trees solved from LLB definitions (llb.go)
}

// bkOutput is an export of the result's files: its type and its index among the
// solve's exports (the client's session finds the destination by it).
type bkOutput struct {
	kind  string
	index int
}

var bkSolves = struct {
	sync.Mutex
	m map[string]*bkSolve
}{m: map[string]*bkSolve{}}

func bkSolveFor(ref string) *bkSolve {
	reject(ref != "", "A build needs a Ref")
	bkSolves.Lock()
	defer bkSolves.Unlock()
	st := bkSolves.m[ref]
	if st == nil {
		st = &bkSolve{progress: newProgress(), ready: make(chan struct{}), returned: make(chan struct{})}
		bkSolves.m[ref] = st
	}
	return st
}

// bkControlSolve is Control.Solve: it records the exports and waits for the
// gateway build to return. buildx's internal solves (reading capabilities) return
// without building.
func (b *engine) bkControlSolve(ctx context.Context, req pbFields) pbuf {
	ref := req.str(1)
	st := bkSolveFor(ref)
	defer func() {
		st.progress.finish()
		time.AfterFunc(time.Minute, func() { // a late Status call still reads the build
			bkSolves.Lock()
			delete(bkSolves.m, ref)
			bkSolves.Unlock()
			st.mu.Lock()
			if st.llb.work != "" {
				go os.RemoveAll(st.llb.work)
			}
			st.mu.Unlock()
		})
	}()
	reject(len(req[2]) == 0 && req.str(6) == "", "Dockiva Engine builds Dockerfiles through buildx (docker build) only, not LLB definitions or other frontends")
	exports := req[13]
	if len(exports) == 0 && req.str(3) != "" { // older clients: ExporterDeprecated, ExporterAttrsDeprecated
		exports = []pbValue{{b: pbuf{}.str(1, req.str(3)).strMap(2, req.strMap(4))}}
	}
	for i, x := range exports {
		e := pbDecode(x.b)
		if kind := e.str(1); kind != "moby" {
			reject(kind == "local" || kind == "tar", "Dockiva Engine exports builds as images (--load, --push), a directory (--output type=local) or a tar file (--output type=tar), not --output type="+kind)
			st.outputs = append(st.outputs, bkOutput{kind, i})
			continue
		}
		for _, name := range strings.Split(e.strMap(2)["name"], ",") {
			if name = strings.TrimSpace(name); name != "" {
				st.names = append(st.names, name)
			}
		}
	}
	st.session = req.str(5)
	st.readyOnce.Do(func() { close(st.ready) })
	select {
	case <-st.returned:
	case <-ctx.Done():
		panic(grpcError{1, "Build cancelled"})
	}
	st.mu.Lock()
	defer st.mu.Unlock()
	if st.failure != "" {
		panic(grpcError{2, st.failure})
	}
	if st.id == "" {
		return nil
	}
	return pbuf{}.strMap(1, map[string]string{"containerimage.digest": st.id, "containerimage.config.digest": st.id, "image.name": strings.Join(st.names, ",")})
}

// bkGatewaySolve is LLBBridge.Solve with the Dockerfile frontend: the whole build,
// exported under the requested names. The result is a reference to the image.
// ponytail: the image is tagged here, before Return; BuildKit exports after it.
func (b *engine) bkGatewaySolve(ctx context.Context, ref string, req pbFields) pbuf {
	st := bkSolveFor(ref)
	select {
	case <-st.ready:
	case <-ctx.Done():
		panic(grpcError{1, "Build cancelled"})
	case <-time.After(30 * time.Second):
		panic(apiError{400, "The build " + ref + " was not started with Control.Solve"})
	}
	if req.str(2) == "" && len(req[1]) > 0 { // a definition: a remote context's files, read before the build
		return b.llbSolve(ctx, st, req.sub(1))
	}
	reject(req.str(2) == "dockerfile.v0", "Dockiva Engine runs Dockerfile builds only, not frontend "+strconv.Quote(req.str(2)))
	reject(len(req[1]) == 0 && len(req[13]) == 0, "LLB definitions and frontend inputs (a --build-context that is a build target or an OCI layout) need BuildKit; Dockiva Engine's builder does not support them")
	id := b.bkBuild(ctx, st, req.strMap(3))
	st.mu.Lock()
	st.id = id
	st.mu.Unlock()
	return pbuf{}.msg(3, pbuf{}.msg(3, pbuf{}.str(1, id))) // Result{ref: Ref{id}}
}

// bkReturn is LLBBridge.Return: the end of the gateway build, with its error.
func bkReturn(ref string, req pbFields) {
	st := bkSolveFor(ref)
	if len(req[2]) > 0 { // google.rpc.Status
		if s := req.sub(2); s.num(1) != 0 {
			st.mu.Lock()
			st.failure = s.str(2)
			if st.failure == "" {
				st.failure = "The build failed"
			}
			st.mu.Unlock()
		}
	}
	st.returnOnce.Do(func() { close(st.returned) })
}

// bkProgress is a build's progress as BuildKit StatusResponse messages, kept for
// Status calls that start late.
type bkProgress struct {
	mu   sync.Mutex
	id   string
	n    int
	msgs [][]byte
	wake chan struct{}
	done bool
}

func newProgress() *bkProgress {
	return &bkProgress{id: token(), wake: make(chan struct{})}
}

func (p *bkProgress) add(m pbuf) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.msgs = append(p.msgs, m)
	close(p.wake)
	p.wake = make(chan struct{})
}

func (p *bkProgress) finish() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if !p.done {
		p.done = true
		close(p.wake)
		p.wake = make(chan struct{})
	}
}

// stream sends the messages so far, then new ones until the build ends.
func (p *bkProgress) stream(ctx context.Context, w http.ResponseWriter) {
	for i := 0; ; {
		p.mu.Lock()
		msgs, done, wake := p.msgs[i:], p.done, p.wake
		p.mu.Unlock()
		for _, m := range msgs {
			grpcWrite(w, m)
		}
		i += len(msgs)
		if done {
			return
		}
		if len(msgs) == 0 {
			select {
			case <-wake:
			case <-ctx.Done():
				return
			}
		}
	}
}

// bkVertex is one step as buildx shows it.
type bkVertex struct {
	p       *bkProgress
	digest  string
	name    string
	started time.Time
	cached  bool
}

func (p *bkProgress) vertex(name string) *bkVertex {
	p.mu.Lock()
	p.n++
	n := p.n
	p.mu.Unlock()
	v := &bkVertex{p: p, digest: digest([]byte(p.id + "/" + strconv.Itoa(n))), name: name, started: time.Now()}
	v.send(time.Time{}, "")
	return v
}

func (v *bkVertex) send(completed time.Time, failure string) {
	v.p.add(pbuf{}.msg(1, pbuf{}.str(1, v.digest).str(3, v.name).flag(4, v.cached).time(5, v.started).time(6, completed).str(7, failure)))
}
func (v *bkVertex) done(failure string) { v.send(time.Now(), failure) }
func (v *bkVertex) log(s string) {
	v.p.add(pbuf{}.msg(3, pbuf{}.str(1, v.digest).time(2, time.Now()).num(3, 1).bytes(4, []byte(s))))
}

// status is a finished sub-step, such as "transferring context:" with a byte count.
func (v *bkVertex) status(id string, current int64) {
	now := time.Now()
	v.p.add(pbuf{}.msg(2, pbuf{}.str(1, id).str(2, v.digest).num(4, uint64(current)).time(6, now).time(7, now).time(8, now)))
}
