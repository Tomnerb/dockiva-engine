package main

import (
	"bufio"
	"bytes"
	"cmp"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
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
	filter  *logFilter    // docker logs -t, --since, --until (nil: output as written)
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
			data := buf[:n]
			if l.filter != nil {
				data = l.filter.lines(i, data, l.offsets[i])
			}
			l.offsets[i] += int64(n)
			if l.raw {
				out = append(out, data...)
			} else {
				out = frame(out, i+1, data)
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

// backoff paces a polling loop: a short interval first (a short run ends within
// milliseconds of starting, and docker run waits for that), doubling to max while
// nothing happens, and short again after activity.
type backoff struct{ next, min, max time.Duration }

func newBackoff(min, max time.Duration) *backoff { return &backoff{min, min, max} }
func (b *backoff) after() <-chan time.Time {
	d := b.next
	b.next = min(b.next*2, b.max)
	return time.After(d)
}
func (b *backoff) reset() { b.next = b.min }

// follow writes new output (checking within 2 ms at first, every 25 ms when idle)
// until done reports the run is over (then drains what is left) or ctx ends. done
// must not panic.
func (l *logReader) follow(ctx context.Context, write func([]byte) error, done func() bool) {
	defer l.close()
	pace := newBackoff(2*time.Millisecond, cmp.Or(l.every, 25*time.Millisecond))
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
			pace.reset()
		}
		if finished {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-pace.after():
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
func (b *engine) followLogs(w http.ResponseWriter, r *http.Request, p string, stdout, stderr bool, offsets [2]int64, raw bool, filter *logFilter) {
	reader := &logReader{dir: p, want: [2]bool{stdout, stderr}, offsets: offsets, raw: raw, filter: filter}
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
		return s == nil || !live(s) || filter != nil && filter.until != 0 && time.Now().UnixNano() > filter.until
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
	pace := newBackoff(2*time.Millisecond, 25*time.Millisecond) // a status read is cheap; docker run waits on this
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
			if s != nil && !live(s) {
				select { // ended: have the watcher report it and remove it now
				case watchNow <- struct{}{}:
				default:
				}
			}
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
		case <-pace.after():
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
func labelAttributes(labels, attributes obj) obj {
	for k, v := range labels {
		attributes[k] = v
	}
	return attributes
}
func containerEvent(action, name, image string, attributes obj) obj {
	t := time.Now()
	attributes["name"], attributes["image"] = name, image
	return obj{"status": action, "id": key(name), "from": image, "Type": "container", "Action": action,
		"Actor": obj{"ID": key(name), "Attributes": attributes}, "scope": "local", "time": t.Unix(), "timeNano": t.UnixNano()}
}

// watch publishes container events and removes exited --rm containers.
// watchNow asks the watcher for a pass at once (docker run --rm waits for the removal
// it does, which would otherwise wait for the next 250 ms tick).
var watchNow = make(chan struct{}, 1)

func (b *engine) watch(stop <-chan struct{}) {
	type seen struct {
		live       bool
		generation any
		image      string
		labels     obj // Docker puts them on each event (docker compose filters by them)
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
				cur := seen{live(s), s["generation"], image, object(c["labels"])}
				current[name] = cur
				before, had := known[name]
				if !first {
					if !had {
						b.events.publish(containerEvent("create", name, image, labelAttributes(cur.labels, obj{})))
					}
					if cur.live && (!had || !before.live || before.generation != cur.generation) {
						b.events.publish(containerEvent("start", name, image, labelAttributes(cur.labels, obj{})))
					}
					if had && before.live && !cur.live {
						b.events.publish(containerEvent("die", name, image, labelAttributes(cur.labels, obj{"exitCode": fmt.Sprint(s["exit_code"])})))
					}
				}
				if c["auto_remove"] == true && !cur.live && s["state"] != "created" {
					b.autoRemove(name, s)
				}
			})
		}
		for name, before := range known {
			if _, ok := current[name]; !ok && !first {
				b.events.publish(containerEvent("destroy", name, before.image, labelAttributes(before.labels, obj{})))
			}
		}
		known, first = current, false
		select {
		case <-stop:
			return
		case <-tick.C:
		case <-watchNow:
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
		reject(k == "type" || k == "container" || k == "event" || k == "label", "Unsupported events filter: "+k)
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
			case "label": // key, or key=value; every one must hold, as in Docker
				attributes := object(actor["Attributes"])
				for value := range values {
					k, want, hasValue := strings.Cut(value, "=")
					got, ok := attributes[k].(string)
					if !ok || hasValue && got != want {
						return false
					}
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
	if v == "" || v == "KILL" || v == "SIGKILL" { // not a stop signal (stopSignal refuses it)
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

// stampedLog appends a container stream to its log (stdout.log) and, before the
// bytes, to its index (stdout.idx) the offset and time (Unix nanoseconds; 8 bytes
// little-endian each) of every line that starts in them: docker logs -t, --since
// and --until read it. The supervisor writes both; one entry is 16 bytes a line.
type stampedLog struct {
	mu       sync.Mutex
	log, idx *os.File
	end      int64 // the log's size
	midline  bool  // the last byte written was not a newline
}

func openStampedLog(dir, name string) *stampedLog {
	log, e := os.OpenFile(filepath.Join(dir, name+".log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	check(e)
	idx, e := os.OpenFile(filepath.Join(dir, name+".idx"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	check(e)
	st, e := log.Stat()
	check(e)
	if i, e := idx.Stat(); e == nil && i.Size()%16 != 0 { // a torn last entry (a crash)
		_ = idx.Truncate(i.Size() - i.Size()%16)
	}
	return &stampedLog{log: log, idx: idx, end: st.Size()}
}

func (l *stampedLog) Write(b []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	now, entries := time.Now().UnixNano(), []byte{}
	for i, c := range b {
		if !l.midline {
			entries = binary.LittleEndian.AppendUint64(binary.LittleEndian.AppendUint64(entries, uint64(l.end+int64(i))), uint64(now))
		}
		l.midline = c != '\n'
	}
	if len(entries) > 0 {
		_, _ = l.idx.Write(entries) // the output matters more than its times
	}
	n, e := l.log.Write(b)
	l.end += int64(n)
	return n, e
}

func (l *stampedLog) Close() error { return errors.Join(l.log.Close(), l.idx.Close()) }

// lineTimes reads a log's index forward: the time of the line starting at an offset.
type lineTimes struct {
	f        *os.File // nil: no index (a log written before indexes existed)
	pos      int64    // the next entry's position in the index
	buf      []byte   // whole entries read and not yet used
	fallback int64    // the time when the index has nothing at or after a line (0: now)
}

// openLineTimes starts at the first entry for an offset at or after from.
func openLineTimes(dir string, stream int, from, fallback int64) *lineTimes {
	t := &lineTimes{fallback: fallback}
	f, e := os.OpenFile(filepath.Join(dir, strings.TrimSuffix(logNames[stream], ".log")+".idx"), os.O_RDONLY|syscall.O_NOFOLLOW, 0)
	if e != nil {
		return t
	}
	st, e := f.Stat()
	check(e)
	entry := make([]byte, 8)
	t.f, t.pos = f, 16*int64(sort.Search(int(st.Size()/16), func(k int) bool {
		n, _ := f.ReadAt(entry, int64(k)*16)
		return n < 8 || int64(binary.LittleEndian.Uint64(entry)) >= from
	}))
	return t
}

// at is the time of the line starting at off (offsets asked in increasing order).
// A line with no entry (written by an older engine) takes the next entry's time,
// or the fallback: no earlier than it was written.
func (t *lineTimes) at(off int64) int64 {
	for {
		for len(t.buf) >= 16 {
			o, when := int64(binary.LittleEndian.Uint64(t.buf)), int64(binary.LittleEndian.Uint64(t.buf[8:]))
			if o > off {
				return when
			}
			t.buf = t.buf[16:]
			if o == off {
				return when
			}
		}
		n := 0
		if t.f != nil {
			chunk := make([]byte, 64*16)
			n, _ = t.f.ReadAt(chunk, t.pos)
			n -= n % 16
			t.buf = chunk[:n]
		}
		if n == 0 {
			return cmp.Or(t.fallback, time.Now().UnixNano())
		}
		t.pos += int64(n)
	}
}

func (t *lineTimes) close() {
	if t.f != nil {
		t.f.Close()
	}
}

// logFilter is docker logs -t (timestamps), --since and --until (Unix nanoseconds,
// 0 for none) over the two streams, keeping where each stream is in its lines.
type logFilter struct {
	dir          string
	timestamps   bool
	since, until int64
	times        [2]*lineTimes
	midline      [2]bool // the last bytes seen did not end their line
	skip         [2]bool // that line is outside since..until
}

// rfc3339NanoFixed is Docker's log timestamp: always nine digits of nanoseconds.
const rfc3339NanoFixed = "2006-01-02T15:04:05.000000000Z07:00"

// lines rewrites a stream's log bytes (data, from offset off): every line starting
// in them is dropped when outside since..until, and otherwise, with timestamps,
// prefixed by its time.
func (f *logFilter) lines(stream int, data []byte, off int64) []byte {
	out := make([]byte, 0, len(data))
	for len(data) > 0 {
		if !f.midline[stream] {
			if f.times[stream] == nil { // a stream whose log was missing when docker logs began
				f.times[stream] = openLineTimes(f.dir, stream, off, 0)
			}
			at := f.times[stream].at(off)
			f.skip[stream] = f.since != 0 && at < f.since || f.until != 0 && at > f.until
			if !f.skip[stream] && f.timestamps {
				out = append(time.Unix(0, at).UTC().AppendFormat(out, rfc3339NanoFixed), ' ')
			}
		}
		n := bytes.IndexByte(data, '\n') + 1
		if n == 0 {
			n = len(data)
		}
		if !f.skip[stream] {
			out = append(out, data[:n]...)
		}
		f.midline[stream] = data[n-1] != '\n'
		data, off = data[n:], off+int64(n)
	}
	return out
}

func (f *logFilter) close() {
	for _, t := range f.times {
		if t != nil {
			t.close()
		}
	}
}

// logTime parses --since and --until as Docker's API sends them: Unix seconds with
// an optional fraction ("1700000000.5"), or RFC 3339. "" and "0" mean none (0).
func logTime(v string) int64 {
	if v == "" || v == "0" {
		return 0
	}
	if t, e := time.Parse(time.RFC3339Nano, v); e == nil {
		return t.UnixNano()
	}
	whole, fraction, _ := strings.Cut(v, ".")
	s, e1 := strconv.ParseUint(whole, 10, 63)
	ns, e2 := strconv.ParseUint((fraction + "000000000")[:9], 10, 63)
	reject(e1 == nil && e2 == nil && len(fraction) <= 9 && strings.Trim(fraction, "0123456789") == "" && s < 1<<33, "Invalid time: "+v)
	return int64(s)*1e9 + int64(ns)
}
