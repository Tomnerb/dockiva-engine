package main

import (
	"bufio"
	"cmp"
	"errors"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// execJob is a docker exec: a command started beside a running container.
//
// ponytail: an exec is a second runtime on the container's root, mounts and network
// (files, volumes, localhost ports and Unix sockets are shared), not a process inside
// the container's runtime: it cannot see or signal the container's own processes.
type execJob struct {
	ID, Container, Path    string // Path: Cmd[0] found in the container's PATH
	Cmd, Env               []string
	User, WorkingDir       string
	Tty, Stdin, Out, Err   bool
	mu                     sync.Mutex
	started, running, done bool
	exitCode, pid          int
	terminal               *os.File // pty master while running with Tty
	keep                   *os.File // liveness pipe: the worker ends the exec if it closes
}

const execLimit = 256

func (b *engine) createExec(container string, body obj) obj {
	reject(onlyKeys(body, "AttachStdin", "AttachStdout", "AttachStderr", "Detach", "DetachKeys", "Tty", "ConsoleSize", "Env", "Cmd", "Privileged", "User", "WorkingDir"), "Unsupported exec option")
	reject(body["Privileged"] == nil || body["Privileged"] == false, "Privileged exec is not supported")
	if !live(b.status(b.container(container))) {
		panic(apiError{409, "Container " + container + " is not running"})
	}
	if exists(filepath.Join(b.container(container), "paused")) {
		panic(apiError{409, "Container " + container + " is paused, unpause the container first"})
	}
	j := &execJob{ID: token(), Container: container, Cmd: stringsOf(body["Cmd"]), Env: stringsOf(body["Env"]),
		User: validUser(text(body["User"])), WorkingDir: text(body["WorkingDir"]),
		Tty: body["Tty"] == true, Stdin: body["AttachStdin"] == true, Out: body["AttachStdout"] == true, Err: body["AttachStderr"] == true}
	reject(len(j.Cmd) > 0 && len(j.Cmd) <= 4096, "Exec requires a command")
	validateEnv(j.Env)
	if j.WorkingDir != "" {
		mountTarget(j.WorkingDir)
	}
	p := b.container(container)
	c := readObject(filepath.Join(p, "config.json"))
	env := mergeEnv(stringsOf(c["env"]), j.Env)
	j.Path = lookPath(containerView(p, c), env, cmp.Or(j.WorkingDir, workdir(c)), j.Cmd[0])
	count := 0
	b.execs.Range(func(any, any) bool { count++; return true })
	if count >= execLimit {
		panic(apiError{503, "Too many execs; retry shortly"})
	}
	b.execs.Store(j.ID, j)
	b.emit("exec_create: "+strings.Join(j.Cmd, " "), container, obj{"execID": j.ID})
	return obj{"Id": j.ID}
}

func (b *engine) execJob(id string) *execJob {
	v, ok := b.execs.Load(id)
	if !ok {
		panic(apiError{404, "No such exec instance: " + id})
	}
	return v.(*execJob)
}

func (b *engine) inspectExec(id string) obj {
	j := b.execJob(id)
	j.mu.Lock()
	defer j.mu.Unlock()
	var code any
	if j.done {
		code = j.exitCode
	}
	return obj{"ID": j.ID, "Running": j.running, "ExitCode": code, "Pid": j.pid, "ContainerID": b.idOf(j.Container), "CanRemove": false, "DetachKeys": "",
		"OpenStdin": j.Stdin, "OpenStdout": j.Out, "OpenStderr": j.Err,
		"ProcessConfig": obj{"tty": j.Tty, "entrypoint": j.Cmd[0], "arguments": j.Cmd[1:], "privileged": false, "user": j.User}}
}

// containerCommand resolves argv as docker run does for its entrypoint: the
// command is found in the container's PATH, its mounts included, and a script runs
// under its #! interpreter. argv0 is what the program sees as its name.
func containerCommand(p string, c obj, dir string, env, argv []string) (command []string, argv0 string) {
	view := containerView(p, c)
	command = (&image{Root: view.files.upper, Lower: view.files.lower, Env: env, mounts: view.mounts}).command(append([]string{lookPath(view, env, dir, argv[0])}, argv[1:]...))
	if argv0 = argv[0]; len(command) != len(argv) {
		argv0 = command[0] // a script: its interpreter
	}
	return command, argv0
}

// lookPath finds a command in the container's PATH, as docker exec does.
// A path with a slash is used as is, relative to the working directory dir.
func lookPath(root guestView, env []string, dir, name string) string {
	if path.IsAbs(name) {
		return path.Clean(name)
	}
	if strings.Contains(name, "/") {
		return path.Join(dir, name)
	}
	search := "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
	for _, v := range env {
		if p, ok := strings.CutPrefix(v, "PATH="); ok {
			search = p
		}
	}
	for _, dir := range strings.Split(search, ":") {
		if !strings.HasPrefix(dir, "/") {
			continue
		}
		candidate := path.Join(dir, name)
		ok := safeRun(func() {
			host, found := root.find(resolveGuest(pathName(candidate[1:]), root.link))
			reject(found, "missing")
			st, e := os.Lstat(host)
			reject(e == nil && st.Mode().IsRegular() && st.Mode()&0o111 != 0, "not executable")
		}) == nil
		if ok {
			return candidate
		}
	}
	panic(apiError{404, "exec: \"" + name + "\": executable file not found in $PATH"})
}

// startExec runs an exec. Attached, it streams over the hijacked connection (raw
// with a TTY, else Docker's multiplexed frames) and ends when the command exits.
func (b *engine) startExec(w http.ResponseWriter, r *http.Request, id string, body obj) {
	j := b.execJob(id)
	j.mu.Lock()
	if j.started {
		j.mu.Unlock()
		panic(apiError{409, "Exec " + id + " has already been started"})
	}
	j.started = true
	j.mu.Unlock()
	detach := body["Detach"] == true
	b.upperSync(b.container(j.Container)) // an exec is a run of its own on the layer's directory
	cmd, input, output, errput := b.execCommand(j)
	var size [2]uint16
	if a, ok := body["ConsoleSize"].([]any); ok && len(a) == 2 && !idle(a[0]) {
		size = [2]uint16{uint16(integer(a[0])), uint16(integer(a[1]))}
	}
	if j.Tty && size[0] > 0 {
		resizeTerminal(j.terminal, size[0], size[1])
	}
	func() {
		b.pausing.Lock() // a pause either sees this exec or this exec sees the pause
		defer b.pausing.Unlock()
		check(cmd.Start())
		closeChildEnds(cmd)
		j.mu.Lock()
		j.running, j.pid = true, cmd.Process.Pid
		j.mu.Unlock()
		if exists(filepath.Join(b.container(j.Container), "paused")) {
			_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGSTOP)
		}
	}()
	b.emit("exec_start: "+strings.Join(j.Cmd, " "), j.Container, obj{"execID": j.ID})
	finished := make(chan struct{})
	go func() {
		err := cmd.Wait()
		code := 0
		var exit *exec.ExitError
		if errors.As(err, &exit) {
			code = exit.ExitCode()
			if code < 0 {
				code = 128 + int(exit.Sys().(syscall.WaitStatus).Signal())
			}
		}
		j.keep.Close()
		j.mu.Lock()
		j.running, j.done, j.exitCode = false, true, code
		j.mu.Unlock()
		b.emit("exec_die", j.Container, obj{"execID": j.ID, "exitCode": strconv.Itoa(code)})
		close(finished)
		time.AfterFunc(10*time.Minute, func() { b.execs.Delete(j.ID) })
	}()
	if detach {
		go drain(output, errput, input, finished, j)
		send(w, 200, nil)
		return
	}
	hijacker, ok := w.(http.Hijacker)
	reject(ok, "Exec requires a hijackable connection")
	conn, rw, e := hijacker.Hijack()
	check(e)
	defer conn.Close()
	_ = conn.SetDeadline(time.Time{})
	content := "application/vnd.docker.multiplexed-stream"
	if j.Tty {
		content = "application/vnd.docker.raw-stream"
	}
	header := "HTTP/1.1 200 OK\r\nContent-Type: " + content + "\r\n\r\n"
	if r.Header.Get("Upgrade") != "" {
		header = "HTTP/1.1 101 UPGRADED\r\nContent-Type: " + content + "\r\nConnection: Upgrade\r\nUpgrade: tcp\r\n\r\n"
	}
	if _, e := rw.WriteString(header); e != nil || rw.Flush() != nil {
		return
	}
	if j.Stdin {
		go func() {
			feed(rw.Reader, input, j.Tty)
			if j.Tty { // the client's terminal went away: hang up, as closing a Linux tty does
				_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGHUP)
			}
		}()
	}
	var mu sync.Mutex
	write := func(stream int, data []byte) {
		mu.Lock()
		defer mu.Unlock()
		if !j.Tty {
			data = frame(nil, stream, data)
		}
		if _, e := rw.Write(data); e == nil {
			_ = rw.Flush()
		}
	}
	var pumps sync.WaitGroup
	for i, out := range []*os.File{output, errput} {
		if out == nil {
			continue
		}
		pumps.Add(1)
		go func(stream int, out *os.File) {
			defer pumps.Done()
			copyTo(out, func(data []byte) {
				write(stream, data)
				select {
				case <-finished: // a slow client took its time: the quiet second starts again
					if j.Tty {
						_ = j.terminal.SetReadDeadline(time.Now().Add(time.Second))
					}
				default:
				}
			})
		}(i+1, out)
	}
	<-finished
	if j.Tty {
		// The pty stays readable until its buffered output is gone, then reports EIO;
		// stop after a quiet second, not a second in all, so no output is cut short.
		_ = j.terminal.SetReadDeadline(time.Now().Add(time.Second))
	}
	pumps.Wait()
	if input != nil {
		input.Close()
	}
	if j.terminal != nil {
		j.terminal.Close()
	}
	if tcp, ok := conn.(interface{ CloseWrite() error }); ok {
		_ = tcp.CloseWrite()
	}
}

// execCommand prepares the worker for an exec: stdin, and stdout and stderr (or a
// pty for all three), returning the engine's ends.
func (b *engine) execCommand(j *execJob) (cmd *exec.Cmd, input, output, errput *os.File) {
	p := b.container(j.Container)
	c := readObject(filepath.Join(p, "config.json"))
	if j.User != "" {
		c["user"] = j.User
	}
	if j.WorkingDir != "" {
		c["workdir"] = j.WorkingDir
	}
	env := mergeEnv(stringsOf(c["env"]), j.Env)
	command, argv0 := containerCommand(p, c, workdir(c), env, j.Cmd)
	extra := []string{}
	if j.Stdin || j.Tty {
		extra = append(extra, "--stdin")
	}
	invocation := runtimeCommand(p, b.Native, c, command, argv0, env, false, extra...)
	cmd = exec.Command(b.Worker, invocation...)
	cmd.Env = []string{"PATH=/usr/bin:/bin", "LANG=C"}
	// The worker ends the exec if the engine goes away (its end of this pipe closes).
	alive, keep, e := os.Pipe()
	check(e)
	cmd.ExtraFiles, j.keep = []*os.File{alive}, keep
	if j.Tty {
		master, slave := openTerminal()
		j.terminal = master
		cmd.Stdin, cmd.Stdout, cmd.Stderr = slave, slave, slave
		cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true, Setctty: true, Ctty: 0}
		return cmd, master, master, nil
	}
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if j.Stdin {
		pair, e := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
		check(e)
		input = os.NewFile(uintptr(pair[0]), "exec-stdin")
		cmd.Stdin = os.NewFile(uintptr(pair[1]), "exec-stdin-child")
	} else {
		null, e := os.Open(os.DevNull)
		check(e)
		cmd.Stdin = null
	}
	if j.Out {
		var w *os.File
		output, w, e = os.Pipe()
		check(e)
		cmd.Stdout = w
	}
	if j.Err {
		var w *os.File
		errput, w, e = os.Pipe()
		check(e)
		cmd.Stderr = w
	}
	return cmd, input, output, errput
}

// closeChildEnds closes the engine's copies of the files the worker inherited.
func closeChildEnds(cmd *exec.Cmd) {
	for _, v := range []any{cmd.Stdin, cmd.Stdout, cmd.Stderr} {
		if f, ok := v.(*os.File); ok {
			f.Close()
		}
	}
	for _, f := range cmd.ExtraFiles {
		f.Close()
	}
}

func mergeEnv(base, extra []string) []string {
	merged := map[string]string{}
	order := []string{}
	for _, v := range append(append([]string{}, base...), extra...) {
		k, val, _ := strings.Cut(v, "=")
		if _, exists := merged[k]; !exists {
			order = append(order, k)
		}
		merged[k] = val
	}
	out := []string{}
	for _, k := range order {
		out = append(out, k+"="+merged[k])
	}
	return out
}

// feed copies the client's input to the command. Without a TTY the client's
// half-close becomes end of input; with one, input simply stops.
func feed(from *bufio.Reader, to *os.File, tty bool) {
	_, _ = io.Copy(to, from)
	if !tty {
		if c, e := net.FileConn(to); e == nil {
			_ = c.(*net.UnixConn).CloseWrite()
			c.Close()
		}
	}
}

func copyTo(from *os.File, write func([]byte)) {
	buffer := make([]byte, 32<<10)
	for {
		n, e := from.Read(buffer)
		if n > 0 {
			write(append([]byte(nil), buffer[:n]...))
		}
		if e != nil {
			return
		}
	}
}

// drain discards a detached exec's output so it never blocks on a full pipe.
func drain(output, errput, input *os.File, finished chan struct{}, j *execJob) {
	for _, f := range []*os.File{output, errput} {
		if f != nil && f != j.terminal {
			go copyTo(f, func([]byte) {})
		}
	}
	if j.terminal != nil {
		go copyTo(j.terminal, func([]byte) {})
	}
	<-finished
	if input != nil && input != j.terminal {
		input.Close()
	}
}

// openTerminal opens a pseudo-terminal pair (macOS: /dev/ptmx).
func openTerminal() (master, slave *os.File) {
	master, e := os.OpenFile("/dev/ptmx", os.O_RDWR|syscall.O_NOCTTY, 0)
	check(e)
	fd := master.Fd()
	for _, request := range []uintptr{0x20007454, 0x20007452} { // TIOCPTYGRANT, TIOCPTYUNLK
		if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, fd, request, 0); errno != 0 {
			master.Close()
			check(errno)
		}
	}
	var name [128]byte
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, fd, 0x40807453, uintptr(unsafe.Pointer(&name[0]))); errno != 0 { // TIOCPTYGNAME
		master.Close()
		check(errno)
	}
	n := 0
	for n < len(name) && name[n] != 0 {
		n++
	}
	slave, e = os.OpenFile(string(name[:n]), os.O_RDWR|syscall.O_NOCTTY, 0)
	if e != nil {
		master.Close()
		check(e)
	}
	return master, slave
}

func resizeTerminal(f *os.File, height, width uint16) {
	if f == nil || height == 0 || width == 0 {
		return
	}
	size := [4]uint16{height, width, 0, 0}
	_, _, _ = syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), syscall.TIOCSWINSZ, uintptr(unsafe.Pointer(&size[0])))
}

func (b *engine) resizeExec(id string, height, width int64) {
	j := b.execJob(id)
	j.mu.Lock()
	defer j.mu.Unlock()
	reject(height > 0 && height < 65536 && width > 0 && width < 65536, "Invalid terminal size")
	if j.running && j.terminal != nil {
		resizeTerminal(j.terminal, uint16(height), uint16(width))
	}
}
