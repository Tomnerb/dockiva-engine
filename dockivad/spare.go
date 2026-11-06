package main

import (
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

// A spare is a supervisor process started ahead of time. A container's start
// hands it the container over a socket instead of starting this 13 MB binary
// then (about 5 ms on an idle Mac, more under load). sparePool wait at a time:
// docker compose starts its services one after another, a few ms apart, and one
// spare took ~6 ms to replace, so every other start found none.
// ponytail: a fixed pool; each idle spare is ~9 MB (supervisor, guard, runtime) and
// one of the Mac's ~128 hypervisor VMs. Size it by recent starts if bursts grow.
const sparePool = 2

type spare struct {
	cmd  *exec.Cmd
	conn *net.UnixConn
}

type spareSlot struct {
	sync.Mutex
	on      bool // set by main: tests run supervisors by exec, from the binary they built
	ready   []*spare
	filling int // spares being started
}

// refillSpare starts spares in the background until sparePool wait or are on the way.
func (b *engine) refillSpare() {
	if !b.spares.on {
		return
	}
	b.spares.Lock()
	missing := sparePool - len(b.spares.ready) - b.spares.filling
	b.spares.filling += max(missing, 0)
	b.spares.Unlock()
	for range missing {
		go func() {
			s := b.newSpare()
			b.spares.Lock()
			b.spares.filling--
			if s != nil && len(b.spares.ready) < sparePool {
				b.spares.ready, s = append(b.spares.ready, s), nil
			}
			b.spares.Unlock()
			if s != nil {
				s.discard()
			}
		}()
	}
}

func (b *engine) newSpare() *spare {
	pair, e := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if e != nil {
		return nil
	}
	syscall.CloseOnExec(pair[0])
	syscall.CloseOnExec(pair[1])
	ours, theirs := os.NewFile(uintptr(pair[0]), "spare"), os.NewFile(uintptr(pair[1]), "spare")
	defer ours.Close()
	defer theirs.Close()
	cmd := exec.Command(b.Self, "_supervise_spare", b.Native, b.Worker)
	cmd.ExtraFiles = []*os.File{theirs}
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if cmd.Start() != nil {
		return nil
	}
	c, e := net.FileConn(ours)
	if e != nil {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil
	}
	return &spare{cmd, c.(*net.UnixConn)}
}

// discard ends a spare that will not be used: it exits when its socket closes.
func (s *spare) discard() {
	s.conn.Close()
	_ = s.cmd.Wait()
}

// takeSpare hands a container to the waiting spare, which then runs
// supervise(args...) with lock as fd 3, ready as fd 4, over as fd 5 and log as
// its output. nil: no spare, or it could not take it; the caller starts a
// supervisor itself.
func (b *engine) takeSpare(args, env []string, lock, ready, log, over *os.File) *exec.Cmd {
	b.spares.Lock()
	var s *spare
	if len(b.spares.ready) > 0 {
		s, b.spares.ready = b.spares.ready[0], b.spares.ready[1:] // the oldest: the likeliest to have its VM ready
	}
	b.spares.Unlock()
	if s == nil {
		b.refillSpare()
		return nil
	}
	defer b.refillSpare()
	msg, e := json.Marshal(spareJob{args, env})
	if e == nil {
		rights := syscall.UnixRights(int(lock.Fd()), int(ready.Fd()), int(log.Fd()), int(over.Fd()))
		var n int
		n, _, e = s.conn.WriteMsgUnix(msg, rights, nil)
		if e == nil && n != len(msg) {
			e = syscall.EIO
		}
	}
	s.conn.Close() // the spare has it all, or exits on EOF
	if e != nil {
		_ = s.cmd.Process.Kill()
		_ = s.cmd.Wait()
		return nil
	}
	return s.cmd
}

type spareJob struct{ Args, Env []string }

// A prestart is the guard and hypervisor runtime a spare starts while it waits:
// the runtime then waits too (dockiva-hvf prestart), for its run's arguments, so
// the run's first instruction does not wait for two process launches (about 7 ms).
type prestart struct {
	cmd      *exec.Cmd
	runtime  string   // the runtime it runs, as runtimeCommand names it
	liveness *os.File // the guard's liveness pipe: the guard ends the runtime when it closes
	job      int      // the socket the runtime reads its run from
}

var prestarted *prestart // the spare's, until its first run takes it or passes

// startPrestart starts the guard and runtime. Its descriptors stay above 100, out
// of the way of the ones a spare's container brings to 1-4.
func startPrestart(native, worker string) *prestart {
	var runtime string
	if safeRun(func() { runtime = regularExecutable(filepath.Join(filepath.Dir(native), "dockiva-hvf")) }) != nil {
		return nil
	}
	reader, writer, e := os.Pipe()
	if e != nil {
		return nil
	}
	defer reader.Close()
	pair, e := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if e != nil {
		writer.Close()
		return nil
	}
	theirs := os.NewFile(uintptr(pair[1]), "prestart-job")
	defer theirs.Close()
	cmd := exec.Command(worker, runtime, "prestart", "--file-limit-mib", "1048576") // the guard's cap: the largest; the run sets its own
	cmd.ExtraFiles = []*os.File{reader, theirs}
	cmd.Env = []string{"PATH=/usr/bin:/bin", "LANG=C"}
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if cmd.Start() != nil {
		writer.Close()
		syscall.Close(pair[0])
		return nil
	}
	return &prestart{cmd: cmd, runtime: runtime, liveness: highFile(writer), job: highFd(pair[0])}
}

func sameFile(a, b string) bool {
	if a == b {
		return true
	}
	x, e := os.Stat(a)
	y, f := os.Stat(b)
	return e == nil && f == nil && os.SameFile(x, y)
}

func highFd(fd int) int {
	r, _, errno := syscall.Syscall(syscall.SYS_FCNTL, uintptr(fd), syscall.F_DUPFD_CLOEXEC, 100)
	if errno != 0 {
		return fd
	}
	syscall.Close(fd)
	return int(r)
}
func highFile(f *os.File) *os.File {
	fd, e := syscall.Dup(int(f.Fd()))
	if e != nil {
		return f
	}
	f.Close()
	return os.NewFile(uintptr(highFd(fd)), f.Name())
}

// end lets an unused prestart go: the guard sees its liveness pipe close and ends
// the runtime.
func (r *prestart) end() {
	syscall.Close(r.job)
	r.liveness.Close()
	go func() { _ = r.cmd.Wait() }()
}

// prestartedRun hands a run to the spare's prestarted runtime when it runs the
// same runtime, without a terminal or input (those need the launch's own stdio).
// A named container's runtime is a hard link to it (processName): the prestarted
// one then execs itself by that name once it has the run.
// It returns the guard, its liveness pipe, a wait for the run's output to be
// copied, and the runtime's exit report: one exit status, sent once the run is over
// and before the runtime's own teardown, or closed without one (it then ends
// another way: the guard's exit tells). nil when the run is to be launched as usual.
func prestartedRun(invocation []string, c obj, out, errout io.Writer) (*exec.Cmd, *os.File, func(), <-chan int) {
	r := prestarted
	prestarted = nil
	if r == nil {
		return nil, nil, nil, nil
	}
	if !sameFile(invocation[0], r.runtime) || c["tty"] == true || c["open_stdin"] == true {
		r.end()
		return nil, nil, nil, nil
	}
	var reads, writes []*os.File
	closeAll := func(files []*os.File) {
		for _, f := range files {
			f.Close()
		}
	}
	var e error
	for range 3 { // stdout, stderr, the exit report
		var rd, wr *os.File
		if rd, wr, e = os.Pipe(); e != nil {
			break
		}
		reads, writes = append(reads, rd), append(writes, wr)
	}
	if e == nil {
		payload := []byte(strings.Join(invocation, "\x00") + "\x00")
		msg := binary.NativeEndian.AppendUint32(nil, uint32(len(payload)))
		msg = append(msg, payload...)
		e = syscall.Sendmsg(r.job, msg[:4], syscall.UnixRights(int(writes[0].Fd()), int(writes[1].Fd()), int(writes[2].Fd())), nil, 0)
		for sent := 4; e == nil && sent < len(msg); {
			var n int
			n, e = syscall.Write(r.job, msg[sent:])
			sent += n
		}
	}
	closeAll(writes)
	if e != nil {
		closeAll(reads)
		r.end()
		return nil, nil, nil, nil
	}
	syscall.Close(r.job)
	var copies sync.WaitGroup
	for i, to := range []io.Writer{out, errout} {
		copies.Add(1)
		go func() { defer copies.Done(); defer reads[i].Close(); _, _ = io.Copy(to, reads[i]) }()
	}
	drained := func() { // as cmd.WaitDelay: output a leftover process holds does not delay the run's end
		ch := make(chan struct{})
		go func() { copies.Wait(); close(ch) }()
		select {
		case <-ch:
		case <-time.After(time.Second):
		}
	}
	report := make(chan int, 1)
	go func() {
		defer reads[2].Close()
		var b [1]byte
		if n, _ := reads[2].Read(b[:]); n == 1 {
			report <- int(b[0])
		}
		close(report)
	}()
	return r.cmd, r.liveness, drained, report
}

// superviseSpare is a spare's life: wait for a container, then supervise it.
func superviseSpare(native, worker string) error {
	// Hold fds 4 and 5 (on /dev/null, its standard input) until the container's
	// ready and over pipes replace them below, so nothing opened meanwhile, the Go
	// runtime's kqueue among them, takes those numbers.
	check(syscall.Dup2(0, 4))
	check(syscall.Dup2(0, 5))
	prestarted = startPrestart(native, worker)
	f := os.NewFile(3, "spare")
	c, e := net.FileConn(f)
	f.Close() // fd 3 is the container lock's below
	check(e)
	conn := c.(*net.UnixConn)
	buf := make([]byte, 1<<20)
	oob := make([]byte, syscall.CmsgSpace(4*4))
	n, oobn, _, _, e := conn.ReadMsgUnix(buf, oob)
	conn.Close()
	if e != nil || n == 0 {
		if prestarted != nil {
			prestarted.end()
			_ = prestarted.cmd.Wait()
		}
		return nil // the engine went on without it
	}
	msgs, e := syscall.ParseSocketControlMessage(oob[:oobn])
	check(e)
	reject(len(msgs) == 1, "Spare supervisor: expected one control message")
	fds, e := syscall.ParseUnixRights(&msgs[0])
	check(e)
	reject(len(fds) == 4, "Spare supervisor: expected four descriptors")
	var job spareJob
	check(json.Unmarshal(buf[:n], &job))
	reject(len(job.Args) == 4, "Invalid supervisor arguments")
	// Move them out of the way first: one may have arrived as 3 or 4.
	high := make([]int, len(fds))
	for i, fd := range fds {
		r, _, errno := syscall.Syscall(syscall.SYS_FCNTL, uintptr(fd), syscall.F_DUPFD_CLOEXEC, 100)
		if errno != 0 {
			check(errno)
		}
		high[i] = int(r)
		syscall.Close(fd)
	}
	// As the exec'd supervisor gets them: lock 3, ready 4, log 1 and 2, over 5.
	for i, target := range []int{3, 4, 1, 2, 5} {
		check(syscall.Dup2(high[[]int{0, 1, 2, 2, 3}[i]], target))
	}
	for _, fd := range high {
		syscall.Close(fd)
	}
	for _, kv := range job.Env {
		if k, v, ok := strings.Cut(kv, "="); ok {
			check(os.Setenv(k, v))
		}
	}
	return supervise(job.Args[0], job.Args[1], job.Args[2], job.Args[3])
}
