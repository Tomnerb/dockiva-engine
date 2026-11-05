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
// then (about 5 ms on an idle Mac, more under load). One waits at a time.
type spare struct {
	cmd  *exec.Cmd
	conn *net.UnixConn
}

type spareSlot struct {
	sync.Mutex
	on   bool // set by main: tests run supervisors by exec, from the binary they built
	next *spare
}

// refillSpare starts the next spare in the background, unless one waits.
func (b *engine) refillSpare() {
	if !b.spares.on {
		return
	}
	go func() {
		s := b.newSpare()
		if s == nil {
			return
		}
		b.spares.Lock()
		if b.spares.next == nil {
			b.spares.next, s = s, nil
		}
		b.spares.Unlock()
		if s != nil {
			s.discard()
		}
	}()
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
// supervise(args...) with lock as fd 3, ready as fd 4 and log as its output.
// nil: no spare, or it could not take it; the caller starts a supervisor itself.
func (b *engine) takeSpare(args, env []string, lock, ready, log *os.File) *exec.Cmd {
	b.spares.Lock()
	s := b.spares.next
	b.spares.next = nil
	b.spares.Unlock()
	if s == nil {
		b.refillSpare()
		return nil
	}
	defer b.refillSpare()
	msg, e := json.Marshal(spareJob{args, env})
	if e == nil {
		rights := syscall.UnixRights(int(lock.Fd()), int(ready.Fd()), int(log.Fd()))
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
// It returns the guard, its liveness pipe and a wait for the run's output to be
// copied; nil when the run is to be launched as usual.
func prestartedRun(invocation []string, c obj, out, errout io.Writer) (*exec.Cmd, *os.File, func()) {
	r := prestarted
	prestarted = nil
	if r == nil {
		return nil, nil, nil
	}
	if invocation[0] != r.runtime || c["tty"] == true || c["open_stdin"] == true {
		r.end()
		return nil, nil, nil
	}
	var outR, outW, errR, errW *os.File
	var e error
	if outR, outW, e = os.Pipe(); e == nil {
		if errR, errW, e = os.Pipe(); e != nil {
			outR.Close()
			outW.Close()
		}
	}
	if e == nil {
		payload := []byte(strings.Join(invocation, "\x00") + "\x00")
		msg := binary.NativeEndian.AppendUint32(nil, uint32(len(payload)))
		msg = append(msg, payload...)
		e = syscall.Sendmsg(r.job, msg[:4], syscall.UnixRights(int(outW.Fd()), int(errW.Fd())), nil, 0)
		for sent := 4; e == nil && sent < len(msg); {
			var n int
			n, e = syscall.Write(r.job, msg[sent:])
			sent += n
		}
		outW.Close()
		errW.Close()
		if e != nil {
			outR.Close()
			errR.Close()
		}
	}
	if e != nil {
		r.end()
		return nil, nil, nil
	}
	syscall.Close(r.job)
	var copies sync.WaitGroup
	for _, p := range []struct {
		to   io.Writer
		from *os.File
	}{{out, outR}, {errout, errR}} {
		copies.Add(1)
		go func() { defer copies.Done(); defer p.from.Close(); _, _ = io.Copy(p.to, p.from) }()
	}
	drained := func() { // as cmd.WaitDelay: output a leftover process holds does not delay the run's end
		ch := make(chan struct{})
		go func() { copies.Wait(); close(ch) }()
		select {
		case <-ch:
		case <-time.After(time.Second):
		}
	}
	return r.cmd, r.liveness, drained
}

// superviseSpare is a spare's life: wait for a container, then supervise it.
func superviseSpare(native, worker string) error {
	prestarted = startPrestart(native, worker)
	f := os.NewFile(3, "spare")
	c, e := net.FileConn(f)
	f.Close() // fd 3 is the container lock's below
	check(e)
	conn := c.(*net.UnixConn)
	buf := make([]byte, 1<<20)
	oob := make([]byte, syscall.CmsgSpace(3*4))
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
	reject(len(fds) == 3, "Spare supervisor: expected three descriptors")
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
	for i, target := range []int{3, 4, 1, 2} { // as the exec'd supervisor gets them
		check(syscall.Dup2(high[min(i, 2)], target))
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
