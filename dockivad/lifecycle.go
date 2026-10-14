package main

import (
	"bufio"
	"context"
	"crypto/subtle"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

var namePattern = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9_-]{0,39}$`)

type engine struct {
	Home, State, Cache, Native, Worker, Assets, Self string
	MaxContainers                                    int
	Mu                                               sync.Mutex
	events                                           events
	exits                                            sync.Map // exit status of auto-removed containers, by name, for a minute
	execs                                            sync.Map // *execJob by ID
}

// memoryMiB is a container's guest memory limit: docker run -m, else its profile's.
func memoryMiB(c obj) int64 {
	for _, k := range []string{"memory_mib", "memory_limit_mib"} {
		if c[k] != nil {
			return integer(c[k])
		}
	}
	return 256
}

func (b *engine) container(name string) string {
	reject(namePattern.MatchString(name), "Invalid container name")
	p := filepath.Join(b.State, name)
	if exists(p) {
		s, e := os.Lstat(p)
		check(e)
		reject(s.IsDir() && s.Mode()&os.ModeSymlink == 0, "Invalid container directory")
	}
	return p
}
func live(s obj) bool {
	return s["state"] == "running" || s["state"] == "starting" || s["state"] == "restarting"
}
func workerAlive(s obj) bool {
	if !live(s) && s["state"] != "failed" {
		return false
	}
	if s["pid"] == nil {
		return false
	}
	pid := integer(s["pid"])
	return pid > 0 && syscall.Kill(int(pid), 0) == nil
}
func (b *engine) status(p string) obj {
	s := readObject(filepath.Join(p, "status.json"))
	if !live(s) {
		return s
	}
	f, e := lockFile(filepath.Join(p, "lock"), true)
	if e == syscall.EWOULDBLOCK || e == syscall.EAGAIN {
		return s
	}
	check(e)
	defer f.Close()
	// Never signal a PID read from stale disk state. A Go worker's liveness pipe
	// closes when its supervisor dies. A legacy live worker needs manual recovery.
	s = readObject(filepath.Join(p, "status.json"))
	if live(s) {
		s["state"] = "failed"
		s["exit_code"] = 125
		s["finished_at"] = now()
		s["error"] = "Supervisor ownership lost; worker recovery required if PID is still alive"
		save(filepath.Join(p, "status.json"), s)
	}
	return s
}
func (b *engine) names() []string {
	rows, e := os.ReadDir(b.State)
	check(e)
	out := []string{}
	for _, r := range rows {
		if r.IsDir() && namePattern.MatchString(r.Name()) && exists(filepath.Join(b.State, r.Name(), "status.json")) {
			out = append(out, r.Name())
		}
	}
	return out
}
func (b *engine) resolve(id string) string {
	for _, n := range b.names() {
		if n == id {
			return n
		}
	}
	out := []string{}
	if len(id) >= 12 {
		for _, n := range b.names() {
			if strings.HasPrefix(key(n), id) {
				out = append(out, n)
			}
		}
	}
	if len(out) != 1 {
		panic(apiError{404, "No such container"})
	}
	return out[0]
}
func settledContainerLock(p string) (*os.File, error) {
	deadline := time.Now().Add(time.Second)
	for {
		f, err := lockFile(filepath.Join(p, "lock"), true)
		if err != syscall.EWOULDBLOCK && err != syscall.EAGAIN {
			return f, err
		}
		if live(readObject(filepath.Join(p, "status.json"))) || time.Now().After(deadline) {
			return nil, err
		}
		// Terminal status precedes final descriptor/lock cleanup.
		time.Sleep(20 * time.Millisecond)
	}
}
func (b *engine) start(name string) {
	p := b.container(name)
	f, e := settledContainerLock(p)
	if e == syscall.EWOULDBLOCK || e == syscall.EAGAIN {
		s := readObject(filepath.Join(p, "status.json"))
		if live(s) {
			return
		}
		panic(apiError{409, "Container operation in progress"})
	}
	check(e)
	defer f.Close()
	s := readObject(filepath.Join(p, "status.json"))
	if workerAlive(s) {
		panic(apiError{409, "Previous worker is still alive; refusing duplicate launch"})
	}
	gate, e := lockFile(filepath.Join(b.State, ".capacity.lock"), false)
	check(e)
	defer gate.Close()
	count := 1
	c := readObject(filepath.Join(p, "config.json"))
	validateLaunchProfile(c, b.Native)
	for _, other := range b.names() {
		if other == name {
			continue
		}
		q := b.container(other)
		held, e := lockFile(filepath.Join(q, "lock"), true)
		if e == nil {
			held.Close()
			continue
		}
		if e != syscall.EWOULDBLOCK && e != syscall.EAGAIN {
			check(e)
		}
		count++
	}
	// Like Docker, memory limits are caps, not reservations: guest memory is committed as it is touched.
	if count > b.MaxContainers {
		panic(apiError{409, "At most " + strconv.Itoa(b.MaxContainers) + " running containers (vmfree-engine --max-containers)"})
	}
	generation := token()
	log, e := os.OpenFile(filepath.Join(p, "supervisor.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	check(e)
	defer log.Close()
	cmd := exec.Command(b.Self, "_supervise", p, b.Native, b.Worker, generation)
	cmd.ExtraFiles = []*os.File{f}
	cmd.Stdout = log
	cmd.Stderr = log
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	check(cmd.Start())
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		s = readObject(filepath.Join(p, "status.json"))
		if s["generation"] == generation {
			if s["state"] == "failed" {
				panic(apiError{409, text(s["error"])})
			}
			return
		}
		select {
		case <-done:
			panic(apiError{409, "Supervisor exited; inspect supervisor.log"})
		default:
		}
		time.Sleep(20 * time.Millisecond)
	}
	_ = cmd.Process.Signal(syscall.SIGTERM)
	panic(apiError{409, "Start status timeout; inspect state before retrying"})
}

// stop ends a container's run: its stop signal, then SIGKILL after the grace period
// (seconds, docker stop -t; else Config.StopTimeout or 10 s).
func (b *engine) stop(name string, seconds ...int) {
	p := b.container(name)
	s := b.status(p)
	if !live(s) {
		return
	}
	defer func() {
		if failure := recover(); failure != nil {
			// Natural exit can remove the control endpoint after our running
			// snapshot. Stop is successful once that same run is terminal.
			deadline := time.Now().Add(time.Second)
			for time.Now().Before(deadline) {
				terminal := false
				err := safeRun(func() { terminal = !live(b.status(p)) })
				if err == nil && terminal {
					return
				}
				time.Sleep(20 * time.Millisecond)
			}
			panic(failure)
		}
	}()
	control := readObject(filepath.Join(p, "control.json"))
	port := integer(control["port"])
	reject(port > 0 && port <= 65535, "Invalid control port")
	conn, e := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.FormatInt(port, 10)), 5*time.Second)
	check(e)
	defer conn.Close()
	grace := 3600 // the supervisor replies once the grace period is over at most
	command := "stop"
	if len(seconds) > 0 {
		grace, command = seconds[0], fmt.Sprintf("stop %d", seconds[0])
	}
	check(conn.SetDeadline(time.Now().Add(time.Duration(grace+5) * time.Second)))
	_, e = fmt.Fprintf(conn, "%s %s\n", str(control["token"]), command)
	check(e)
	var reply [2]byte
	n, e := io.ReadFull(conn, reply[:])
	check(e)
	reject(n == 2 && string(reply[:]) == "OK", "Supervisor did not confirm stop")
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if !live(b.status(p)) {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	panic(apiError{409, "Stop status timeout"})
}

// restartOnBoot starts, as the engine starts, the containers whose run ended without
// it (the Mac restarted, say) and whose policy asks for it: "always", and
// "unless-stopped" unless the user stopped them.
func (b *engine) restartOnBoot() {
	for _, name := range b.names() {
		_ = safeRun(func() {
			p := b.container(name)
			policy := object(readObject(filepath.Join(p, "config.json"))["restart"])["name"]
			s := b.status(p)
			if live(s) || s["state"] == "created" || !(policy == "always" || policy == "unless-stopped" && s["stopped_by_user"] != true) {
				return
			}
			b.Mu.Lock()
			defer b.Mu.Unlock()
			b.start(name)
		})
	}
}

// control opens a command connection to a running container's supervisor.
func (b *engine) control(p, command string) net.Conn {
	control := readObject(filepath.Join(p, "control.json"))
	port := integer(control["port"])
	reject(port > 0 && port <= 65535, "Invalid control port")
	conn, e := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.FormatInt(port, 10)), 5*time.Second)
	check(e)
	if _, e := fmt.Fprintf(conn, "%s %s\n", str(control["token"]), command); e != nil {
		conn.Close()
		check(e)
	}
	return conn
}

// resize sets a running terminal container's size (docker run -t; best effort).
func (b *engine) resize(name string, rows, columns int64) {
	p := b.container(name)
	if !live(b.status(p)) || readObject(filepath.Join(p, "config.json"))["tty"] != true {
		return
	}
	_ = safeRun(func() {
		conn := b.control(p, fmt.Sprintf("resize %d %d", rows, columns))
		defer conn.Close()
		_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
		_, _ = io.ReadFull(conn, make([]byte, 2))
	})
}

// feedStdin waits for the container's run (the one in progress, or the next) and
// copies in to its standard input, then ends the input with a half-close.
func (b *engine) feedStdin(ctx context.Context, p string, initial obj, in io.Reader) error {
	for {
		s := b.statusOf(p)
		if s == nil {
			return nil
		}
		if live(s) && (live(initial) || s["generation"] != initial["generation"]) && exists(filepath.Join(p, "control.json")) {
			break
		}
		select {
		case <-ctx.Done():
			return nil
		case <-time.After(20 * time.Millisecond):
		}
	}
	var conn net.Conn
	if e := safeRun(func() { conn = b.control(p, "stdin") }); e != nil {
		return nil // the run ended first
	}
	defer conn.Close()
	_, e := io.Copy(conn, in)
	_ = conn.(*net.TCPConn).CloseWrite()
	return e
}

func (b *engine) remove(name string) {
	p := b.container(name)
	f, e := settledContainerLock(p)
	if e != nil {
		panic(apiError{409, "Container is active"})
	}
	defer f.Close()
	s := readObject(filepath.Join(p, "status.json"))
	reject(!workerAlive(s), "Previous worker is still alive")
	network := text(readObject(filepath.Join(p, "config.json"))["net_name"])
	check(os.RemoveAll(p))
	b.refreshHosts(network)
}
func sandboxArgs(native string, args []string, root string, writable bool) []string {
	literal := func(s string) string {
		for _, r := range s {
			reject(r >= 32 && r < 127, "Sandbox paths must be printable ASCII")
		}
		return strconv.Quote(s)
	}
	policy := "(version 1)\n(deny default)\n(allow sysctl-read)\n(allow process-exec (literal " + literal(native) + "))\n(allow file-read* (subpath \"/usr/lib\") (subpath \"/System/Library\") (subpath \"/System/Volumes/Preboot/Cryptexes/OS\") (literal " + literal(native) + ") (subpath " + literal(root) + "))\n(allow file-read* file-write* (literal \"/dev/null\"))\n(allow network-outbound (remote tcp \"localhost:*\"))\n"
	parents := map[string]bool{}
	for _, p := range []string{native, root} {
		for {
			p = filepath.Dir(p)
			parents[p] = true
			if p == "/" {
				break
			}
		}
	}
	policy += "(allow file-read-metadata"
	for p := range parents {
		policy += " (literal " + literal(p) + ")"
	}
	policy += ")\n"
	if writable {
		policy += "(allow file-write* (subpath " + literal(root) + "))"
	}
	return append([]string{"/usr/bin/sandbox-exec", "-p", policy, native}, args...)
}

// runtimeCommand is the worker's argument list that runs command (its first element
// an absolute guest path, run as argv0) in container p with config c. A container's
// own run publishes its ports; an exec beside it does not.
func runtimeCommand(p, native string, c obj, command []string, argv0 string, env []string, publish bool, extra ...string) []string {
	validateLaunchProfile(c, native)
	reject(len(command) > 0 && strings.HasPrefix(command[0], "/"), "Invalid guest command")
	reject(c["cpu_backend"] == nil || c["cpu_backend"] == "interpreter" || c["cpu_backend"] == "hvf", "Unknown execution backend")
	root := noLinks(p, "rootfs")
	// Resolve guest symlinks inside root, then confirm the result is link-free.
	guest := noLinks(root, resolveGuest(pathName(command[0][1:]), func(p string) (string, bool) {
		s, e := os.Lstat(filepath.Join(root, p))
		if e != nil || s.Mode()&os.ModeSymlink == 0 {
			return "", false
		}
		target, e := os.Readlink(filepath.Join(root, p))
		check(e)
		return target, true
	}))
	validateEnv(env)
	steps := "10000000"
	if c["max_steps"] != nil {
		n, ok := c["max_steps"].(json.Number)
		reject(ok, "Invalid instruction limit")
		_, e := strconv.ParseUint(n.String(), 10, 64)
		check(e)
		steps = n.String()
	}
	memory := memoryMiB(c)
	reject(memory >= 1 && memory <= 65536, "Invalid memory limit")
	// The run creates the oom file when it kills a process over the memory limit
	// (exec and healthchecks share the container's budget, as in a cgroup).
	args := []string{"run", guest, "--root", root, "--argv0", argv0, "--max-steps", steps, "--memory-limit-mib", strconv.FormatInt(memory, 10), "--oom-file", filepath.Join(p, "oom")}
	// The container's own run publishes its process table; exec and healthcheck runs
	// show it in their /proc (ps, top), and docker top reads it.
	if publish {
		args = append(args, "--proc-publish", filepath.Join(p, "procs"))
	} else {
		args = append(args, "--proc-peer", filepath.Join(p, "procs"))
	}
	if c["cpu_backend"] == "hvf" {
		native = regularExecutable(filepath.Join(filepath.Dir(native), "vmfree-c-hvf"))
		// Like Docker, a container may use every core: one vCPU per host CPU.
		cpus := min(runtime.NumCPU(), 64)
		if c["cpus"] != nil {
			cpus = min(cpus, int(integer(c["cpus"]))) // docker run --cpus
		}
		args = append(args, "--cpu-backend", "hvf", "--cpus", strconv.Itoa(cpus))
	}
	if wd := workdir(c); wd != "/" {
		args = append(args, "--cwd", wd)
	}
	// Stock binaries exceed the 16 MiB ELF default (node is 110 MiB, traefik 150 MiB).
	// The mmap arena is virtual: pages count against the memory limit on first write.
	// Every container gets the largest (12 GiB from 12 GiB up): a JVM reserves its heap,
	// a 1 GiB class space that must start on a 4 GiB boundary and code cache beyond -m,
	// and needs a third aligned slot free after libraries and the heap take two.
	args = append(args, "--elf-limit-mib", "512", "--mmap-limit-mib", "12288")
	// Like Docker, disk use is left to the host: no storage budget, 4 GiB per file.
	args = append(args, "--file-limit-mib", "4096", "--storage-limit-mib", "0")
	reject(c["network"] == nil || c["network"] == "none" || c["network"] == "outbound", "Invalid network mode")
	// Docker gives every container /dev/shm (POSIX shared memory and semaphores) and
	// /proc; these directories stand in (the runtime serves /proc/self/fd/N itself).
	for _, d := range []string{"dev/shm", "proc/self/fd"} {
		_ = safeRun(func() { mountPoint(root, d, true) })
	}
	_ = os.Chmod(filepath.Join(root, "dev/shm"), os.ModeSticky|0o777)
	home := filepath.Dir(filepath.Dir(p)) // <home>/containers/<name>
	if c["network"] != "none" {
		name := filepath.Base(p)
		members := []obj{}
		if n, ok := c["net_name"].(string); ok && n != "bridge" {
			members = membersOn(home, n)
		}
		networkFiles(root, hostsFile(home, name, c, members))
		args = append(args, "--outbound")
	}
	args = append(args, "--hostname", hostname(filepath.Base(p), c))
	if ip, ok := c["ip"].(string); ok && c["network"] != "none" {
		args = append(args, "--network", networkPorts(home, str(c["net_name"])), "--ip", ip)
	}
	for _, p := range publishedPorts(c) {
		if publish {
			args = append(args, "--publish", strconv.FormatInt(p[0], 10)+":"+strconv.FormatInt(p[1], 10))
		}
	}
	// <home>/containers/<name>: volumes are in <home>/volumes.
	args = append(args, prepareMounts(filepath.Dir(filepath.Dir(p)), root, c)...)
	for _, pair := range [][2]string{{"schedule", "--schedule"}, {"process_tree", "--process-tree"}, {"scheduled_network", "--scheduled-network"}, {"dynamic", "--dynamic"}, {"job_control", "--job-control"}, {"writable", "--writable"}} {
		v := c[pair[0]]
		reject(v == nil || v == true || v == false, "Invalid runtime flag")
		if v == true {
			args = append(args, pair[1])
		}
	}
	// Process trees get Linux signal handlers and docker stop/kill delivery to PID 1;
	// --job-control adds a terminal on top and already includes them.
	if c["process_tree"] == true && c["job_control"] != true {
		args = append(args, "--signals")
	}
	for _, v := range env {
		args = append(args, "--env", v)
	}
	args = append(args, userArgs(root, c, env)...)
	args = append(args, extra...)
	args = append(args, "--")
	args = append(args, command[1:]...)
	if c["host_sandbox"] == true {
		return sandboxArgs(native, args, root, c["writable"] == true)
	}
	return append([]string{native}, args...)
}

func supervise(p, native, worker, generation string) (result error) {
	lock := os.NewFile(3, "container-lock")
	defer lock.Close()
	st, err := lock.Stat()
	check(err)
	reject(st.Mode().IsRegular(), "Supervisor requires an inherited container lock")
	check(syscall.Flock(3, syscall.LOCK_EX|syscall.LOCK_NB))
	status := obj{"state": "starting", "generation": generation, "supervisor": "dockivad", "supervisor_pid": os.Getpid()}
	var listener net.Listener
	var cmd *exec.Cmd
	var writer *os.File
	var done chan error
	finished := false
	defer func() {
		if v := recover(); v != nil {
			status["state"] = "failed"
			status["error"] = fmt.Sprint(v)
			status["exit_code"] = 125
			status["finished_at"] = now()
			result = fmt.Errorf("%v", v)
		}
		if done != nil && !finished {
			_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
			<-done
		}
		if writer != nil {
			writer.Close()
		}
		if listener != nil {
			listener.Close()
		}
		_ = os.Remove(filepath.Join(p, "control.json"))
		if e := safeRun(func() { save(filepath.Join(p, "status.json"), status) }); e != nil {
			result = e
		}
	}()
	c := readObject(filepath.Join(p, "config.json"))
	command := stringsOf(c["command"])
	env := stringsOf(c["env"])
	clearPorts(filepath.Dir(filepath.Dir(p)), c) // left by a run that did not end cleanly
	extra := []string{}
	if c["tty"] == true || c["open_stdin"] == true {
		extra = append(extra, "--stdin")
	}
	invocation := runtimeCommand(p, native, c, command, command[0], env, true, extra...)
	stop := syscall.SIGTERM
	if name, ok := c["stop_signal"].(string); ok && name != "" {
		stop = stopSignals[stopSignal(name)]
	}
	listenerErr := error(nil)
	listener, listenerErr = net.Listen("tcp", "127.0.0.1:0")
	check(listenerErr)
	secret := token()
	save(filepath.Join(p, "control.json"), obj{"port": listener.Addr().(*net.TCPAddr).Port, "token": secret})
	out, e := os.OpenFile(filepath.Join(p, "stdout.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	check(e)
	defer out.Close()
	errout, e := os.OpenFile(filepath.Join(p, "stderr.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	check(e)
	defer errout.Close()
	signals := make(chan os.Signal, 2)
	signal.Notify(signals, syscall.SIGTERM, syscall.SIGINT)
	defer signal.Stop(signals)
	grace := 10 * time.Second // Docker's default; Config.StopTimeout or docker stop -t
	if c["stop_timeout"] != nil {
		grace = time.Duration(integer(c["stop_timeout"])) * time.Second
	}
	// docker run -i/-t: the supervisor holds the container's input, and a terminal's
	// output, which it appends to stdout.log; attach and resize reach it by control.
	var input, terminal *os.File
	var copied, healthStop chan struct{}
	var started time.Time
	// launch starts a run: the first, or a restart under the container's policy.
	launch := func() {
		finished = false
		reader, w, e := os.Pipe() // the worker's liveness pipe: it ends the run if we go
		check(e)
		writer = w
		defer reader.Close()
		cmd = exec.Command(worker, invocation...)
		cmd.ExtraFiles = []*os.File{reader}
		cmd.Stdout = out
		cmd.Stderr = errout
		cmd.Env = []string{"PATH=/usr/bin:/bin", "LANG=C"}
		cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
		var child *os.File
		if c["tty"] == true {
			terminal, child = openTerminal()
			input = terminal
			if a, ok := c["console_size"].([]any); ok && len(a) == 2 {
				resizeTerminal(terminal, uint16(integer(a[0])), uint16(integer(a[1])))
			}
			cmd.Stdin, cmd.Stdout, cmd.Stderr = child, child, child
			cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true, Setctty: true}
		} else if c["open_stdin"] == true {
			pair, e := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
			check(e)
			input, child = os.NewFile(uintptr(pair[0]), "stdin"), os.NewFile(uintptr(pair[1]), "container-stdin")
			cmd.Stdin = child
		}
		_ = os.Remove(filepath.Join(p, "oom")) // OOMKilled describes the latest run
		check(cmd.Start())
		if child != nil {
			child.Close()
		}
		copied = make(chan struct{})
		if terminal != nil {
			go func(terminal *os.File, copied chan struct{}) { _, _ = io.Copy(out, terminal); close(copied) }(terminal, copied) // ends (EIO) once the run closes it
		} else {
			close(copied)
		}
		done = make(chan error, 1)
		go func(cmd *exec.Cmd, done chan error) { done <- cmd.Wait() }(cmd, done)
		started = time.Now()
		status["state"] = "running"
		status["pid"] = cmd.Process.Pid
		status["started_at"] = now()
		save(filepath.Join(p, "status.json"), status)
		if c["healthcheck"] != nil {
			healthStop = make(chan struct{})
			go watchHealth(p, native, worker, c, healthStop)
		}
	}
	finish := func() int {
		finished = true
		if healthStop != nil {
			close(healthStop)
			healthStop = nil
		}
		select { // a terminal's last output reaches the log before the run is reported over
		case <-copied:
		case <-time.After(2 * time.Second):
		}
		// Also cover an externally killed guard: no worker may outlive its run.
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		code := cmd.ProcessState.ExitCode()
		if code < 0 {
			code = 128 + int(cmd.ProcessState.Sys().(syscall.WaitStatus).Signal())
		}
		clearPorts(filepath.Dir(filepath.Dir(p)), c)
		if input != nil {
			input.Close()
			input, terminal = nil, nil
		}
		writer.Close()
		writer = nil
		status["state"] = "exited"
		status["exit_code"] = code
		status["finished_at"] = now()
		return code
	}
	terminate := func(grace time.Duration) {
		if finished { // between restarts: nothing runs
			return
		}
		// The image's StopSignal, then SIGKILL after the grace period.
		_ = syscall.Kill(-cmd.Process.Pid, stop)
		select {
		case <-done:
		case <-time.After(grace):
			_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
			<-done
		}
		finish()
	}
	launch()
	restarts, delay := 0, time.Duration(0)
	var restartAt <-chan time.Time
	for {
		select {
		case <-done:
			code := finish()
			var again bool
			if delay, again = restartDelay(c, code, restarts, time.Since(started), delay); !again {
				return nil
			}
			status["state"] = "restarting"
			save(filepath.Join(p, "status.json"), status)
			restartAt = time.After(delay)
		case <-restartAt:
			restartAt = nil
			restarts++
			status["restart_count"] = restarts
			launch()
		case <-signals:
			terminate(grace)
			return nil
		default:
		}
		check(listener.(*net.TCPListener).SetDeadline(time.Now().Add(100 * time.Millisecond)))
		conn, e := listener.Accept()
		if ne, ok := e.(net.Error); ok && ne.Timeout() {
			continue
		}
		check(e)
		_ = conn.SetDeadline(time.Now().Add(time.Second))
		lines := bufio.NewReaderSize(conn, 128)
		line, e := lines.ReadSlice('\n')
		// "<token> stdin": the rest of the connection is input for the container.
		if e == nil && input != nil && subtle.ConstantTimeCompare(line, []byte(secret+" stdin\n")) == 1 {
			_ = conn.SetDeadline(time.Time{})
			go func(conn net.Conn, input *os.File, tty bool) {
				defer conn.Close()
				_, _ = io.Copy(input, lines)
				if c["stdin_once"] == true && !tty { // the attached client is done: end of input
					_ = syscall.Shutdown(int(input.Fd()), syscall.SHUT_WR)
				}
			}(conn, input, terminal != nil)
			continue
		}
		// "<token> resize <rows> <columns>": the attached terminal's size.
		resize := []byte(secret + " resize ")
		if e == nil && terminal != nil && len(line) > len(resize) && subtle.ConstantTimeCompare(line[:len(resize)], resize) == 1 {
			var rows, columns uint16
			if _, err := fmt.Sscanf(string(line[len(resize):]), "%d %d", &rows, &columns); err == nil {
				resizeTerminal(terminal, rows, columns)
				_, _ = conn.Write([]byte("OK"))
			}
			conn.Close()
			continue
		}
		// "<token> stop [seconds]": docker stop, with its grace period (-t).
		stopping := []byte(secret + " stop")
		if e == nil && len(line) <= 128 && len(line) > len(stopping) && subtle.ConstantTimeCompare(line[:len(stopping)], stopping) == 1 {
			wait := grace
			if n, err := strconv.Atoi(strings.TrimSpace(string(line[len(stopping):]))); err == nil && n >= 0 && n <= 3600 {
				wait = time.Duration(n) * time.Second
			}
			terminate(wait)
			status["state"] = "exited" // also between restarts
			status["stopped_by_user"] = true
			_, _ = conn.Write([]byte("OK"))
			conn.Close()
			return nil
		}
		// "<token> kill <signal>": docker kill. The runtime forwards the signal to the
		// container's first process; SIGKILL ends the run (seen as done above).
		prefix := []byte(secret + " kill ")
		if e == nil && !finished && len(line) > len(prefix) && subtle.ConstantTimeCompare(line[:len(prefix)], prefix) == 1 {
			n, err := strconv.Atoi(strings.TrimSpace(string(line[len(prefix):])))
			allowed := err == nil && syscall.Signal(n) == syscall.SIGKILL
			for _, s := range stopSignals {
				allowed = allowed || (err == nil && s == syscall.Signal(n) && s != syscall.SIGWINCH)
			}
			if allowed && syscall.Kill(-cmd.Process.Pid, syscall.Signal(n)) == nil {
				_, _ = conn.Write([]byte("OK"))
			}
		}
		conn.Close()
	}
}

// networkFiles gives the container Docker's /etc/resolv.conf and /etc/hosts.
// Guest sockets are host sockets, so the Mac's IPv4 nameservers (loopback ones
// included) work unchanged, and host.docker.internal is the shared loopback.
func networkFiles(root, hosts string) {
	host, _ := os.ReadFile("/etc/resolv.conf") // macOS keeps it current; missing means fallback
	writeGuestFile(root, "etc/resolv.conf", resolvConf(host))
	writeGuestFile(root, "etc/hosts", hosts)
}

// resolvConf keeps the host's IPv4 nameservers and search domains. Without any,
// it uses Docker's fallback resolvers.
func resolvConf(host []byte) string {
	var servers, search []string
	for _, line := range strings.Split(string(host), "\n") {
		f := strings.Fields(line)
		if len(f) >= 2 && f[0] == "nameserver" {
			if ip := net.ParseIP(f[1]); ip != nil && ip.To4() != nil && len(servers) < 3 {
				servers = append(servers, ip.To4().String())
			}
		} else if len(f) >= 2 && (f[0] == "search" || f[0] == "domain") {
			search = f[1:]
		}
	}
	if len(servers) == 0 {
		servers = []string{"8.8.8.8", "8.8.4.4"}
	}
	out := ""
	for _, s := range servers {
		out += "nameserver " + s + "\n"
	}
	if len(search) > 0 {
		out += "search " + strings.Join(search, " ") + "\n"
	}
	return out
}

// writeGuestFile replaces a regular file (or a symlink, never followed) inside a
// container root whose parent directories contain no symlinks.
func writeGuestFile(root, relative, content string) {
	dir := filepath.Dir(relative)
	if _, e := os.Lstat(filepath.Join(root, dir)); os.IsNotExist(e) {
		check(os.MkdirAll(filepath.Join(root, dir), 0o755))
	}
	p := filepath.Join(noLinks(root, dir), filepath.Base(relative))
	if s, e := os.Lstat(p); e == nil {
		reject(s.Mode().IsRegular() || s.Mode()&os.ModeSymlink != 0, "Cannot replace "+relative)
		check(os.Remove(p))
	}
	f, e := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, 0o644)
	check(e)
	defer f.Close()
	_, e = f.WriteString(content)
	check(e)
}
