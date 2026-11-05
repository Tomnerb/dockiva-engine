package main

import (
	"encoding/json"
	"net"
	"os"
	"os/exec"
	"strings"
	"sync"
	"syscall"
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
	cmd := exec.Command(b.Self, "_supervise_spare")
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

// superviseSpare is a spare's life: wait for a container, then supervise it.
func superviseSpare() error {
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
