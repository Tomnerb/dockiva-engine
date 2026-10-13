package main

import (
	"bufio"
	"cmp"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"syscall"
	"time"
)

// Long-lived responses (logs -f, attach, wait, events) outlive the server's read
// and write timeouts, which are lifted for that one request.
func unbounded(w http.ResponseWriter) {
	rc := http.NewResponseController(w)
	_ = rc.SetReadDeadline(time.Time{})
	_ = rc.SetWriteDeadline(time.Time{})
}

// frame appends data as Docker multiplexed-stream frames (1 stdout, 2 stderr).
func frame(out []byte, stream int, data []byte) []byte {
	for len(data) > 0 {
		n := min(len(data), 16384)
		header := make([]byte, 8)
		header[0] = byte(stream)
		binary.BigEndian.PutUint32(header[4:], uint32(n))
		out = append(append(out, header...), data[:n]...)
		data = data[n:]
	}
	return out
}

// logReader follows a container's stdout.log and stderr.log from given offsets.
// Files are opened once they exist and stay open, so output written just before
// an auto-removed container's directory goes is still read.
type logReader struct {
	dir     string
	want    [2]bool
	files   [2]*os.File
	offsets [2]int64
	raw     bool          // a terminal's output: unframed
	every   time.Duration // polling interval (default 100 ms)
}

var logNames = [2]string{"stdout.log", "stderr.log"}

func logSizes(dir string) [2]int64 {
	var sizes [2]int64
	for i, name := range logNames {
		if st, e := os.Lstat(filepath.Join(dir, name)); e == nil && st.Mode().IsRegular() {
			sizes[i] = st.Size()
		}
	}
	return sizes
}

// next returns the framed output written since the last call.
func (l *logReader) next() []byte {
	out := []byte{}
	buf := make([]byte, 65536)
	for i := range logNames {
		if !l.want[i] {
			continue
		}
		if l.files[i] == nil {
			f, e := os.OpenFile(filepath.Join(l.dir, logNames[i]), os.O_RDONLY|syscall.O_NOFOLLOW, 0)
			if e != nil {
				continue
			}
			l.files[i] = f
		}
		for len(out) < 1<<20 {
			n, _ := l.files[i].ReadAt(buf, l.offsets[i])
			if n <= 0 {
				break
			}
			l.offsets[i] += int64(n)
			if l.raw {
				out = append(out, buf[:n]...)
			} else {
				out = frame(out, i+1, buf[:n])
			}
		}
	}
	return out
}

// openNow opens (creating, if the container has not run yet) the wanted logs and
// starts at their current end. The supervisor appends to the same files, and an
// open file stays readable after its container's directory is removed (--rm).
func (l *logReader) openNow() {
	for i, name := range logNames {
		if !l.want[i] {
			continue
		}
		f, e := os.OpenFile(filepath.Join(l.dir, name), os.O_RDONLY|os.O_CREATE|syscall.O_NOFOLLOW, 0600)
		check(e)
		st, e := f.Stat()
		check(e)
		l.files[i], l.offsets[i] = f, st.Size()
	}
}
func (l *logReader) close() {
	for _, f := range l.files {
		if f != nil {
			f.Close()
		}
	}
}

// follow writes new output every 100 ms until done reports the run is over (then
// drains what is left) or ctx ends. done must not panic.
func (l *logReader) follow(ctx context.Context, write func([]byte) error, done func() bool) {
	defer l.close()
	tick := time.NewTicker(cmp.Or(l.every, 100*time.Millisecond))
	defer tick.Stop()
	for {
		finished := done()
		for {
			chunk := l.next()
			if len(chunk) == 0 {
				break
			}
			if write(chunk) != nil {
				return
			}
		}
		if finished {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
	}
}

// statusOf reads a container's status, or nil once the container is gone.
func (b *engine) statusOf(p string) (s obj) {
	if safeRun(func() { s = b.status(p) }) != nil {
		return nil
	}
	return s
}

// attach streams a container's output from now on to a hijacked connection:
// until the run in progress ends (docker attach), or, for a container not yet
// running (docker run without -d), until the run that starts after the attach ends.
// With stdin, the client's input goes to the container's standard input (docker
// run -i, attach) once its run has started; a terminal's output is sent raw.
func (b *engine) attach(w http.ResponseWriter, r *http.Request, name string, stdin, stdout, stderr bool) {
	p := b.container(name)
	initial := b.status(p)
	c := readObject(filepath.Join(p, "config.json"))
	tty := c["tty"] == true
	reader := &logReader{dir: p, want: [2]bool{stdout, stderr && !tty}, raw: tty}
	if tty {
		reader.every = 15 * time.Millisecond // keystroke echo
	}
	reader.openNow()
	hijacker, ok := w.(http.Hijacker)
	reject(ok, "Attach requires a hijackable connection")
	conn, rw, e := hijacker.Hijack()
	check(e)
	defer conn.Close()
	_ = conn.SetDeadline(time.Time{})
	upgrade := r.Header.Get("Upgrade") != ""
	content := "application/vnd.docker.multiplexed-stream"
	if tty {
		content = "application/vnd.docker.raw-stream"
	}
	header := "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.docker.raw-stream\r\n\r\n"
	if upgrade {
		header = "HTTP/1.1 101 UPGRADED\r\nContent-Type: " + content + "\r\nConnection: Upgrade\r\nUpgrade: tcp\r\n\r\n"
	}
	if _, e := rw.WriteString(header); e != nil || rw.Flush() != nil {
		return
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() {
		if stdin && c["open_stdin"] == true {
			// The client's input, then its half-close (end of input with docker run -i).
			if e := b.feedStdin(ctx, p, initial, rw.Reader); e != nil {
				cancel()
			}
			return
		}
		// Without stdin the docker CLI half-closes its side at once (EOF): the stream
		// goes on until the run ends or a write fails. Any other read error ends it.
		if _, e := rw.ReadByte(); e != nil && e != io.EOF {
			cancel()
		}
	}()
	reader.follow(ctx, func(chunk []byte) error {
		if _, e := rw.Write(chunk); e != nil {
			return e
		}
		return rw.Flush()
	}, func() bool {
		s := b.statusOf(p)
		return s == nil || (!live(s) && (live(initial) || s["generation"] != initial["generation"]))
	})
}

// followLogs streams existing output (already written by the caller) and then
// new output until the container is no longer running.
func (b *engine) followLogs(w http.ResponseWriter, r *http.Request, p string, stdout, stderr bool, offsets [2]int64, raw bool) {
	reader := &logReader{dir: p, want: [2]bool{stdout, stderr}, offsets: offsets, raw: raw}
	flusher, _ := w.(http.Flusher)
	reader.follow(r.Context(), func(chunk []byte) error {
		if _, e := w.Write(chunk); e != nil {
			return e
		}
		if flusher != nil {
			flusher.Flush()
		}
		return nil
	}, func() bool {
		s := b.statusOf(p)
		return s == nil || !live(s)
	})
}

// wait blocks until condition holds and reports the exit status. next-exit counts a
// run that ended within the last 2 s as next: docker run may issue its wait just
// after the start that ends the run it is waiting for.
func (b *engine) wait(w http.ResponseWriter, r *http.Request, name, condition string) {
	reject(condition == "" || condition == "not-running" || condition == "next-exit" || condition == "removed", "Unsupported wait condition: "+condition)
	p := b.container(name)
	initial := b.status(p)
	asked := now()
	code := initial["exit_code"]
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(200)
	if f, ok := w.(http.Flusher); ok {
		f.Flush() // the client may start the container once its wait is registered
	}
	tick := time.NewTicker(100 * time.Millisecond)
	defer tick.Stop()
	for {
		s := b.statusOf(p)
		if s != nil && s["exit_code"] != nil {
			code = s["exit_code"]
		}
		if s == nil {
			if exit, ok := b.exits.Load(name); ok {
				code = exit
			}
		}
		var done bool
		switch condition {
		case "removed":
			done = s == nil
		case "next-exit":
			recent := s != nil && s["finished_at"] != nil && asked-floatOf(s["finished_at"]) < 2
			done = s == nil || (!live(s) && (s["generation"] != initial["generation"] || live(initial) || recent))
		default:
			done = s == nil || !live(s)
		}
		if done {
			if code == nil {
				code = 0
			}
			_ = json.NewEncoder(w).Encode(obj{"StatusCode": code, "Error": nil})
			return
		}
		select {
		case <-r.Context().Done():
			return
		case <-tick.C:
		}
	}
}
func floatOf(v any) float64 {
	switch x := v.(type) {
	case json.Number:
		f, _ := x.Float64()
		return f
	case float64:
		return x
	}
	return 0
}

// Container events, derived from state changes seen every 250 ms (supervisors, not
// this daemon, run containers). Subscribers get create, start, die and destroy.
type events struct {
	mu   sync.Mutex
	subs map[chan obj]bool
}

func (e *events) subscribe() chan obj {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.subs == nil {
		e.subs = map[chan obj]bool{}
	}
	ch := make(chan obj, 256)
	e.subs[ch] = true
	return ch
}
func (e *events) unsubscribe(ch chan obj) {
	e.mu.Lock()
	defer e.mu.Unlock()
	delete(e.subs, ch)
}
func (e *events) publish(ev obj) {
	e.mu.Lock()
	defer e.mu.Unlock()
	for ch := range e.subs {
		select {
		case ch <- ev:
		default: // a stalled subscriber misses events rather than blocking the engine
		}
	}
}
func containerEvent(action, name, image string, attributes obj) obj {
	t := time.Now()
	attributes["name"], attributes["image"] = name, image
	return obj{"status": action, "id": key(name), "from": image, "Type": "container", "Action": action,
		"Actor": obj{"ID": key(name), "Attributes": attributes}, "scope": "local", "time": t.Unix(), "timeNano": t.UnixNano()}
}

// watch publishes container events and removes exited --rm containers.
func (b *engine) watch(stop <-chan struct{}) {
	type seen struct {
		live       bool
		generation any
		image      string
	}
	known := map[string]seen{}
	first := true
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for {
		current := map[string]seen{}
		var names []string
		_ = safeRun(func() { names = b.names() })
		for _, name := range names {
			// Each container on its own: one unreadable container must not stall the rest.
			_ = safeRun(func() {
				p := filepath.Join(b.State, name)
				s := b.statusOf(p)
				if s == nil {
					return
				}
				c := readObject(filepath.Join(p, "config.json"))
				image, _ := c["image_name"].(string)
				cur := seen{live(s), s["generation"], image}
				current[name] = cur
				before, had := known[name]
				if !first {
					if !had {
						b.events.publish(containerEvent("create", name, image, obj{}))
					}
					if cur.live && (!had || !before.live || before.generation != cur.generation) {
						b.events.publish(containerEvent("start", name, image, obj{}))
					}
					if had && before.live && !cur.live {
						b.events.publish(containerEvent("die", name, image, obj{"exitCode": fmt.Sprint(s["exit_code"])}))
					}
				}
				if c["auto_remove"] == true && !cur.live && s["state"] != "created" {
					b.autoRemove(name, s)
				}
			})
		}
		for name, before := range known {
			if _, ok := current[name]; !ok && !first {
				b.events.publish(containerEvent("destroy", name, before.image, obj{}))
			}
		}
		known, first = current, false
		select {
		case <-stop:
			return
		case <-tick.C:
		}
	}
}

// autoRemove deletes an exited --rm container, keeping its exit status for a
// docker run that waits for removal.
func (b *engine) autoRemove(name string, s obj) {
	b.Mu.Lock()
	defer b.Mu.Unlock()
	code := s["exit_code"]
	if code == nil {
		code = 0
	}
	var mounts []obj
	_ = safeRun(func() { mounts = savedMounts(readObject(filepath.Join(b.container(name), "config.json"))) })
	if safeRun(func() { b.remove(name) }) == nil {
		b.removeAnonymousVolumes(mounts) // --rm also removes anonymous volumes, as Docker does
		b.exits.Store(name, code)
		time.AfterFunc(time.Minute, func() { b.exits.Delete(name) })
	}
}

// streamEvents sends events as JSON lines until the client goes away.
func (b *engine) streamEvents(w http.ResponseWriter, r *http.Request, filters obj) {
	for k := range filters {
		reject(k == "type" || k == "container" || k == "event", "Unsupported events filter: "+k)
	}
	match := func(ev obj) bool {
		for k, v := range filters {
			values := map[string]bool{}
			switch x := v.(type) {
			case map[string]any:
				for value := range x {
					values[value] = true
				}
			case []any:
				for _, value := range x {
					values[str(value)] = true
				}
			default:
				reject(false, "Invalid events filter")
			}
			actor := object(ev["Actor"])
			name := text(object(actor["Attributes"])["name"])
			id := text(actor["ID"])
			switch k {
			case "type":
				if !values["container"] {
					return false
				}
			case "event":
				if !values[text(ev["Action"])] {
					return false
				}
			case "container":
				found := values[name] || values[id]
				for value := range values {
					found = found || (len(value) >= 12 && len(value) <= len(id) && id[:len(value)] == value)
				}
				if !found {
					return false
				}
			}
		}
		return true
	}
	ch := b.events.subscribe()
	defer b.events.unsubscribe(ch)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(200)
	flusher, _ := w.(http.Flusher)
	if flusher != nil {
		flusher.Flush()
	}
	encoder := json.NewEncoder(w)
	for {
		select {
		case <-r.Context().Done():
			return
		case ev := <-ch:
			if !match(ev) {
				continue
			}
			if encoder.Encode(ev) != nil {
				return
			}
			if flusher != nil {
				flusher.Flush()
			}
		}
	}
}

// signalNumber parses docker kill's signal: a name with or without SIG, or a number.
// Only signals the runtime forwards to the container's first process, and SIGKILL.
func signalNumber(v string) syscall.Signal {
	if v == "" {
		return syscall.SIGKILL
	}
	if n, e := strconv.Atoi(v); e == nil {
		for _, s := range stopSignals {
			if int(s) == n && s != syscall.SIGWINCH {
				return s
			}
		}
		reject(n == int(syscall.SIGKILL), "Unsupported signal: "+v)
		return syscall.SIGKILL
	}
	name := stopSignal(v)
	if name == "SIGKILL" {
		return syscall.SIGKILL
	}
	reject(name != "SIGWINCH", "Unsupported signal: "+v)
	return stopSignals[name]
}

// kill sends a signal to a running container through its supervisor.
func (b *engine) kill(name string, sig syscall.Signal) {
	p := b.container(name)
	s := b.status(p)
	if !live(s) {
		panic(apiError{409, "Container " + name + " is not running"})
	}
	control := readObject(filepath.Join(p, "control.json"))
	port := integer(control["port"])
	reject(port > 0 && port <= 65535, "Invalid control port")
	conn, e := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.FormatInt(port, 10)), 5*time.Second)
	check(e)
	defer conn.Close()
	check(conn.SetDeadline(time.Now().Add(5 * time.Second)))
	_, e = conn.Write([]byte(str(control["token"]) + " kill " + strconv.Itoa(int(sig)) + "\n"))
	check(e)
	reply, e := bufio.NewReader(conn).Peek(2)
	if e != nil || string(reply) != "OK" {
		panic(apiError{409, "Supervisor did not accept the signal (a container started by an older engine needs a restart)"})
	}
}
