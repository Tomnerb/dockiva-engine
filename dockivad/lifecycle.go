package main

import (
	"bufio"
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
	Mu                                               sync.Mutex
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
func live(s obj) bool { return s["state"] == "running" || s["state"] == "starting" }
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
	memory := int64(256)
	if c["memory_limit_mib"] != nil {
		memory = integer(c["memory_limit_mib"])
	}
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
		cc := readObject(filepath.Join(q, "config.json"))
		m := int64(256)
		if cc["memory_limit_mib"] != nil {
			m = integer(cc["memory_limit_mib"])
		}
		memory += m
	}
	// Budgets are upper bounds; guest memory is committed only as it is touched.
	if count > 8 || memory > 4096 {
		panic(apiError{409, "State-store capacity exceeded: at most 8 active runtimes and 4096 MiB of reserved guest allocations"})
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
func (b *engine) stop(name string) {
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
	check(conn.SetDeadline(time.Now().Add(15 * time.Second))) // the supervisor replies after the 10 s grace period at most
	_, e = fmt.Fprintf(conn, "%s stop\n", str(control["token"]))
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
func (b *engine) remove(name string) {
	p := b.container(name)
	f, e := settledContainerLock(p)
	if e != nil {
		panic(apiError{409, "Container is active"})
	}
	defer f.Close()
	s := readObject(filepath.Join(p, "status.json"))
	reject(!workerAlive(s), "Previous worker is still alive")
	check(os.RemoveAll(p))
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
	validateLaunchProfile(c, native)
	command := stringsOf(c["command"])
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
	env := stringsOf(c["env"])
	validateEnv(env)
	steps := "10000000"
	if c["max_steps"] != nil {
		n, ok := c["max_steps"].(json.Number)
		reject(ok, "Invalid instruction limit")
		_, e := strconv.ParseUint(n.String(), 10, 64)
		check(e)
		steps = n.String()
	}
	memory := int64(256)
	if c["memory_limit_mib"] != nil {
		memory = integer(c["memory_limit_mib"])
	}
	reject(memory >= 1 && memory <= 1024, "Invalid memory limit")
	args := []string{"run", guest, "--root", root, "--argv0", command[0], "--max-steps", steps, "--memory-limit-mib", strconv.FormatInt(memory, 10)}
	if c["cpu_backend"] == "hvf" {
		native = regularExecutable(filepath.Join(filepath.Dir(native), "vmfree-c-hvf"))
		// Like Docker, a container may use every core: one vCPU per host CPU.
		args = append(args, "--cpu-backend", "hvf", "--cpus", strconv.Itoa(min(runtime.NumCPU(), 64)))
	}
	if wd := workdir(c); wd != "/" {
		args = append(args, "--cwd", wd)
	}
	// Stock server binaries (redis-server is 17 MiB) exceed the 16 MiB ELF default.
	// The mmap arena is virtual: pages count against --memory-limit-mib on first write.
	args = append(args, "--elf-limit-mib", "64", "--mmap-limit-mib", "2048")
	// Like Docker, disk use is left to the host: no storage budget, 4 GiB per file.
	args = append(args, "--file-limit-mib", "4096", "--storage-limit-mib", "0")
	reject(c["network"] == nil || c["network"] == "none" || c["network"] == "outbound", "Invalid network mode")
	if c["network"] != "none" {
		networkFiles(root, filepath.Base(p))
		args = append(args, "--outbound")
	}
	for _, p := range publishedPorts(c) {
		args = append(args, "--publish", strconv.FormatInt(p[0], 10)+":"+strconv.FormatInt(p[1], 10))
	}
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
	stop := syscall.SIGTERM
	if name, ok := c["stop_signal"].(string); ok && name != "" {
		stop = stopSignals[stopSignal(name)]
	}
	for _, v := range env {
		args = append(args, "--env", v)
	}
	args = append(args, "--")
	args = append(args, command[1:]...)
	invocation := append([]string{native}, args...)
	if c["host_sandbox"] == true {
		invocation = sandboxArgs(native, args, root, c["writable"] == true)
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
	reader, w, e := os.Pipe()
	check(e)
	writer = w
	defer reader.Close()
	signals := make(chan os.Signal, 2)
	signal.Notify(signals, syscall.SIGTERM, syscall.SIGINT)
	defer signal.Stop(signals)
	cmd = exec.Command(worker, invocation...)
	cmd.ExtraFiles = []*os.File{reader}
	cmd.Stdout = out
	cmd.Stderr = errout
	cmd.Env = []string{"PATH=/usr/bin:/bin", "LANG=C"}
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	check(cmd.Start())
	reader.Close()
	done = make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	status["state"] = "running"
	status["pid"] = cmd.Process.Pid
	status["started_at"] = now()
	save(filepath.Join(p, "status.json"), status)
	finish := func() {
		finished = true
		// Also cover an externally killed guard: no worker may outlive its run.
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		code := cmd.ProcessState.ExitCode()
		if code < 0 {
			code = 128 + int(cmd.ProcessState.Sys().(syscall.WaitStatus).Signal())
		}
		status["state"] = "exited"
		status["exit_code"] = code
		status["finished_at"] = now()
	}
	terminate := func() {
		// The image's StopSignal, then SIGKILL after Docker's default 10 s grace period.
		_ = syscall.Kill(-cmd.Process.Pid, stop)
		select {
		case <-done:
		case <-time.After(10 * time.Second):
			_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
			<-done
		}
		finish()
	}
	for {
		select {
		case <-done:
			finish()
			return nil
		case <-signals:
			terminate()
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
		line, e := bufio.NewReaderSize(conn, 128).ReadSlice('\n')
		valid := e == nil && len(line) <= 128 && subtle.ConstantTimeCompare(line, []byte(secret+" stop\n")) == 1
		if valid {
			terminate()
			_, _ = conn.Write([]byte("OK"))
			conn.Close()
			return nil
		}
		conn.Close()
	}
}

// networkFiles gives the container Docker's /etc/resolv.conf and /etc/hosts.
// Guest sockets are host sockets, so the Mac's IPv4 nameservers (loopback ones
// included) work unchanged, and host.docker.internal is the shared loopback.
func networkFiles(root, hostname string) {
	host, _ := os.ReadFile("/etc/resolv.conf") // macOS keeps it current; missing means fallback
	writeGuestFile(root, "etc/resolv.conf", resolvConf(host))
	writeGuestFile(root, "etc/hosts", "127.0.0.1\tlocalhost\n127.0.0.1\t"+hostname+"\n127.0.0.1\thost.docker.internal\n")
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
