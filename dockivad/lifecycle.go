package main

import (
	"bufio"
	"cmp"
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
	"slices"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

// namePattern: container and network names, as Docker accepts them (act and CI tools
// generate long ones with dots); never starting with a dot, so state files stay apart.
var namePattern = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,199}$`)

type engine struct {
	Home, State, Cache, Native, Worker, Assets, Self string
	MaxContainers                                    int
	Insecure                                         bool   // --insecure-local: plain HTTP to loopback registries
	BindIP                                           string // -default-bind-ip: where -p HOST:GUEST publishes ("": 127.0.0.1)
	Mu                                               sync.Mutex
	events                                           events
	exits                                            sync.Map      // exit status of auto-removed containers, by name, for a minute
	execs                                            sync.Map      // *execJob by ID
	pausing                                          sync.Mutex    // pause and unpause, and an exec starting meanwhile
	quit                                             chan struct{} // POST /dockiva/shutdown asks main to exit
	watched                                          watcher       // what engine.watch last saw
	spares                                           spareSlot     // a supervisor started ahead of the next start
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
	if namePattern.MatchString(id) && exists(filepath.Join(b.State, id, "status.json")) { // a name (as names() finds them)
		return id
	}
	out := []string{}
	if len(id) >= 12 {
		for _, n := range b.names() {
			if strings.HasPrefix(b.idOf(n), id) {
				out = append(out, n)
			}
		}
	}
	if len(out) != 1 {
		panic(apiError{404, "No such container"})
	}
	return out[0]
}

// idOf is a container's ID: a hash of its name, or, once renamed, the ID it had
// (kept in its "id" file), as Docker's IDs outlive renames.
func (b *engine) idOf(name string) string {
	if id, e := os.ReadFile(filepath.Join(b.State, name, "id")); e == nil && len(id) == 64 {
		return string(id)
	}
	return key(name)
}

func settledContainerLock(p string) (*os.File, error) {
	deadline := time.Now().Add(time.Second)
	for {
		wake := changed()
		f, err := lockFile(filepath.Join(p, "lock"), true)
		if err != syscall.EWOULDBLOCK && err != syscall.EAGAIN {
			return f, err
		}
		if live(readObject(filepath.Join(p, "status.json"))) || time.Now().After(deadline) {
			return nil, err
		}
		// Terminal status precedes final descriptor/lock cleanup (the supervisor's exit).
		select {
		case <-wake:
		case <-time.After(20 * time.Millisecond):
		}
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
	stopped := b.stoppedStatuses()
	for _, other := range b.names() {
		if other == name {
			continue
		}
		q := b.container(other)
		// Stopped when the watcher last read it and its status not replaced since: no
		// run (a start writes its status under this gate before letting it go; a
		// restart under its policy writes "restarting" first).
		if was, ok := stopped[other]; ok {
			if now, ok := identityOf(filepath.Join(q, "status.json")); ok && now == was {
				continue
			}
		}
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
		panic(apiError{409, "At most " + strconv.Itoa(b.MaxContainers) + " running containers (dockiva-engine --max-containers)"})
	}
	generation := token()
	log, e := os.OpenFile(filepath.Join(p, "supervisor.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	check(e)
	defer log.Close()
	// The supervisor closes its end once the run's status is saved (or it fails).
	ready, readyEnd, e := os.Pipe()
	check(e)
	defer ready.Close()
	// And this one once its last status is saved and the container's lock let go:
	// the container is the engine's again before the supervisor's process is gone.
	over, overEnd, e := os.Pipe()
	if e != nil {
		readyEnd.Close()
		check(e)
	}
	args := []string{p, b.Native, b.Worker, generation}
	env := []string{"DOCKIVA_DEFAULT_BIND_IP=" + b.BindIP, "DOCKIVA_PROBES=" + knownProbes(), "DOCKIVA_READY_FD=4", "DOCKIVA_OVER_FD=5"}
	cmd := b.takeSpare(args, env, f, readyEnd, log, overEnd)
	if cmd == nil {
		cmd = exec.Command(b.Self, append([]string{"_supervise"}, args...)...)
		cmd.ExtraFiles = []*os.File{f, readyEnd, overEnd}
		cmd.Stdout = log
		cmd.Stderr = log
		cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
		cmd.Env = append(os.Environ(), env...)
		if e = cmd.Start(); e != nil {
			readyEnd.Close()
			overEnd.Close()
			over.Close()
			check(e)
		}
	}
	readyEnd.Close()
	overEnd.Close()
	go func() { _, _ = over.Read(make([]byte, 1)); over.Close(); notifyChanged() }() // EOF
	done := make(chan error, 1)
	go func() { done <- cmd.Wait(); notifyChanged() }() // its runs are over (a restart keeps it)
	signaled := make(chan struct{})
	go func() { _, _ = ready.Read(make([]byte, 1)); close(signaled) }() // EOF
	deadline := time.Now().Add(10 * time.Second)
	pace := newBackoff(time.Millisecond, 4*time.Millisecond) // only if the pipe says nothing
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
			// A short run can start, end and take its supervisor with it between two
			// reads: it started if its status names this generation.
			if s = readObject(filepath.Join(p, "status.json")); s["generation"] == generation && s["state"] != "failed" {
				return
			}
			if s["generation"] == generation {
				panic(apiError{409, text(s["error"])})
			}
			panic(apiError{409, "Supervisor exited; inspect supervisor.log"})
		case <-signaled:
			signaled = nil // read status now; then back to pacing
		case <-pace.after():
		}
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
	b.unpause(name)
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
		wake := changed()
		if !live(b.status(p)) {
			return
		}
		select { // the supervisor saves the status as it exits
		case <-wake:
		case <-time.After(20 * time.Millisecond):
		}
	}
	panic(apiError{409, "Stop status timeout"})
}

// pause freezes a running container (docker pause), or thaws it (unpause): SIGSTOP
// or SIGCONT to the process group of its run (worker guard and runtime) and of each
// of its execs, each its own runtime. The "paused" file says so (State.Paused); the
// supervisor skips healthchecks while it is there and removes it as a run ends.
func (b *engine) pause(name string, freeze bool) {
	p := b.container(name)
	s := b.status(p)
	if s["state"] != "running" || s["pid"] == nil {
		panic(apiError{409, "Container " + name + " is not running"})
	}
	b.pausing.Lock()
	defer b.pausing.Unlock()
	marker := filepath.Join(p, "paused")
	if freeze && exists(marker) {
		panic(apiError{409, "Container " + name + " is already paused"})
	}
	if !freeze && !exists(marker) {
		panic(apiError{409, "Container " + name + " is not paused"})
	}
	sig := syscall.SIGCONT
	if freeze {
		sig = syscall.SIGSTOP
		check(os.WriteFile(marker, nil, 0600)) // first: no healthcheck starts in between
	}
	groups := []int{int(integer(s["pid"]))} // the run's group leader (Setpgid, or Setsid with a terminal)
	b.execs.Range(func(_, v any) bool {
		j := v.(*execJob)
		j.mu.Lock()
		if j.Container == name && j.running && j.pid > 0 {
			groups = append(groups, j.pid)
		}
		j.mu.Unlock()
		return true
	})
	for _, g := range groups {
		_ = syscall.Kill(-g, sig) // a group gone meanwhile is fine
	}
	if !freeze {
		check(os.Remove(marker))
	}
}

// unpause thaws a paused container, so stop, kill and remove reach it as usual.
func (b *engine) unpause(name string) {
	if exists(filepath.Join(b.container(name), "paused")) {
		_ = safeRun(func() { b.pause(name, false) }) // not running: the run's end removes it
	}
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

// rename is docker rename: a stopped or created container takes a new name and keeps
// its ID and host name. ponytail: a running container's runtime and supervisor work
// in its directory, which is named after it, so it must be stopped first.
func (b *engine) rename(name, to string) {
	to = strings.TrimPrefix(to, "/")
	reject(namePattern.MatchString(to), "Invalid container name: "+to)
	if to == name {
		panic(apiError{409, "Renaming a container with the same name as its current name"})
	}
	p, q := b.container(name), b.container(to)
	if exists(q) {
		panic(apiError{409, "Conflict. The container name \"/" + to + "\" is already in use"})
	}
	f, e := settledContainerLock(p)
	if e != nil || workerAlive(readObject(filepath.Join(p, "status.json"))) {
		panic(apiError{409, "Container " + name + " is running: Dockiva Engine renames stopped containers (docker stop it first)"})
	}
	defer f.Close()
	id, c := b.idOf(name), readObject(filepath.Join(p, "config.json"))
	if c["hostname"] == nil {
		c["hostname"] = hostname(name, c)
		save(filepath.Join(p, "config.json"), c)
	}
	check(os.WriteFile(filepath.Join(p, "id"), []byte(id), 0600))
	check(os.Rename(p, q))
	b.refreshHosts(text(c["net_name"]))
	b.emit("rename", to, obj{"oldName": "/" + name})
}

// update is docker update: memory (-m), CPUs (--cpus) and the restart policy. A new
// policy holds from the run's next end; limits from the container's next start.
func (b *engine) update(name string, body obj) obj {
	p := b.container(name)
	c := readObject(filepath.Join(p, "config.json"))
	limits := false
	for k, v := range body {
		switch {
		case k == "RestartPolicy" && !idle(object(v)["Name"]): // the CLI always sends one, empty without --restart
			reject(restartPolicyOK(object(v)), "Invalid restart policy")
			setRestart(c, object(v))
		case k == "Memory" || k == "NanoCpus":
			limits = limits || !idle(v)
		case k == "MemorySwap":
			reject(idle(v) || integer(v) >= -1, "Invalid MemorySwap")
		case k != "RestartPolicy" && !idle(v):
			panic(apiError{501, "Unsupported update option: " + k})
		}
	}
	setLimits(c, body)
	save(filepath.Join(p, "config.json"), c)
	warnings := []string{}
	if limits && live(b.status(p)) {
		warnings = append(warnings, "Dockiva Engine applies memory and CPU limits when the container next starts")
	}
	b.emit("update", name, obj{})
	return obj{"Warnings": warnings}
}

// restartPolicyOK: --restart no, always, unless-stopped or on-failure[:N].
func restartPolicyOK(m obj) bool {
	name := text(m["Name"])
	return onlyKeys(m, "Name", "MaximumRetryCount") && slices.Contains([]string{"", "no", "always", "unless-stopped", "on-failure"}, name) &&
		(idle(m["MaximumRetryCount"]) || name == "on-failure" && integer(m["MaximumRetryCount"]) > 0)
}
func setRestart(c, policy obj) {
	if idle(policy["Name"]) || policy["Name"] == "no" {
		delete(c, "restart")
		return
	}
	c["restart"] = obj{"name": policy["Name"], "max": policy["MaximumRetryCount"]}
}

// setLimits applies HostConfig's Memory (docker run -m) and NanoCpus (--cpus).
func setLimits(c, host obj) {
	if !idle(host["Memory"]) {
		mib := (integer(host["Memory"]) + 1<<20 - 1) >> 20
		reject(mib >= 6 && mib <= 65536, "Memory limit must be 6 MiB to 64 GiB")
		c["memory_mib"] = json.Number(strconv.FormatInt(mib, 10))
	}
	if !idle(host["NanoCpus"]) {
		// ponytail: --cpus sets the vCPU count, rounded up, not a CFS quota.
		cpus := (integer(host["NanoCpus"]) + 1e9 - 1) / 1e9
		reject(cpus >= 1 && cpus <= 64, "CPUs must be 0.01 to 64")
		c["cpus"] = json.Number(strconv.FormatInt(cpus, 10))
	}
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
	c := readObject(filepath.Join(p, "config.json"))
	network := text(c["net_name"])
	// The name is free at once; the files (a whole root file system) go in the
	// background. A removal cut short is finished at the next engine start.
	gone := filepath.Join(b.State, ".removing-"+token()[:16])
	check(os.Rename(p, gone))
	notifyChanged()
	go func() { _ = os.RemoveAll(gone) }()
	releaseLower(b.Home, b.State, text(c["lower"])) // an image removed meanwhile: its files go now
	b.refreshHosts(network)
}
func sandboxArgs(native string, args []string, r layers, writable bool) []string {
	root := r.upper
	literal := func(s string) string {
		for _, r := range s {
			reject(r >= 32 && r < 127, "Sandbox paths must be printable ASCII")
		}
		return strconv.Quote(s)
	}
	lowerRule, genRule := "", ""
	if r.lower != "" {
		lowerRule = " (subpath " + literal(r.lower) + ")"
		genRule = "(allow file-read* file-write* (literal " + literal(r.gen) + "))\n" // the layers' change count (overlay.go)
	}
	policy := "(version 1)\n(deny default)\n(allow sysctl-read)\n(allow process-exec (literal " + literal(native) + "))\n(allow file-read* (subpath \"/usr/lib\") (subpath \"/System/Library\") (subpath \"/System/Volumes/Preboot/Cryptexes/OS\") (literal " + literal(native) + ") (subpath " + literal(root) + ")" + lowerRule + ")\n(allow file-read* file-write* (literal \"/dev/null\"))\n(allow network-outbound (remote tcp \"localhost:*\"))\n" + genRule
	parents := map[string]bool{}
	for _, p := range []string{native, root, cmp.Or(r.lower, root)} {
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
// own run publishes its process table; an exec beside it reads it. The supervisor
// adds the run's --publish options (publishPorts) to extra.
func runtimeCommand(p, native string, c obj, command []string, argv0 string, env []string, publish bool, extra ...string) []string {
	validateLaunchProfile(c, native)
	reject(len(command) > 0 && strings.HasPrefix(command[0], "/"), "Invalid guest command")
	reject(c["cpu_backend"] == nil || c["cpu_backend"] == "interpreter" || c["cpu_backend"] == "hvf", "Unknown execution backend")
	r := containerLayers(p, c)
	root := r.upper
	// Resolve guest symlinks inside the container; find refuses a symlink on the way.
	guest, found := r.find(resolveGuest(pathName(command[0][1:]), r.link))
	reject(found, "Executable missing: "+command[0])
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
	args := []string{"run", guest, "--root", root}
	if r.lower != "" {
		// The image's small files from one mapping, built beside its root by the first run.
		args = append(args, "--lower", r.lower, "--layers-gen", r.gen, "--lower-pack", r.lower+".pack")
		// The container's own run keeps the writable layer's new files in memory until
		// it writes them back (runtime upm.h): dockivad asks for them first whenever it
		// reads the layer's directory itself (upperSync). DOCKIVA_UPPER_MEMORY=0 opts out.
		marker := filepath.Join(p, "upper-memory")
		_ = os.Remove(filepath.Join(p, "upper-sync")) // a request no run answered
		if publish && os.Getenv("DOCKIVA_UPPER_MEMORY") != "0" {
			check(os.WriteFile(marker, nil, 0o600))
			args = append(args, "--upper-memory", filepath.Join(p, "upper-sync"))
			if c["auto_remove"] == true {
				args = append(args, "--upper-discard") // docker run --rm: the layer goes with the container
			}
		} else if publish {
			_ = os.Remove(marker)
		}
	}
	args = append(args, "--argv0", argv0, "--max-steps", steps, "--memory-limit-mib", strconv.FormatInt(memory, 10), "--oom-file", filepath.Join(p, "oom"))
	// Abstract Unix socket names live in one directory per container, shared by its execs.
	abstract := filepath.Join(p, "abstract")
	check(os.MkdirAll(abstract, 0o700))
	args = append(args, "--abstract-dir", abstract)
	// The container's own run publishes its process table; exec and healthcheck runs
	// show it in their /proc (ps, top), and docker top reads it.
	if publish {
		args = append(args, "--proc-publish", filepath.Join(p, "procs"))
	} else {
		args = append(args, "--proc-peer", filepath.Join(p, "procs"))
	}
	if c["cpu_backend"] == "hvf" {
		native = regularExecutable(filepath.Join(filepath.Dir(native), "dockiva-hvf"))
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
	args = append(args, "--elf-limit-mib", "1024", "--mmap-limit-mib", "12288")
	// Like Docker, disk use is left to the host: no storage budget, and files of up to
	// 1 TiB (the runtime's largest per-file limit).
	args = append(args, "--file-limit-mib", "1048576", "--storage-limit-mib", "0")
	if mode := os.Getenv("DOCKIVA_FSYNC"); mode != "" { // dockiva-engine --fsync
		args = append(args, "--fsync", mode)
	}
	reject(c["network"] == nil || c["network"] == "none" || c["network"] == "outbound", "Invalid network mode")
	// Docker gives every container /dev/shm (POSIX shared memory and semaphores) and
	// /proc; these directories stand in (the runtime serves /proc/self/fd/N itself).
	for _, d := range []string{"dev/shm", "proc/self/fd"} {
		_ = safeRun(func() { r.mountPoint(d, true) })
	}
	_ = safeRun(func() {
		if host, ok := r.find("dev/shm"); ok && isDir(host) {
			if st, e := os.Lstat(host); e == nil && st.Mode()&(os.ModeSticky|os.ModePerm) != os.ModeSticky|0o777 {
				check(os.Chmod(r.upperDir("dev/shm", true), os.ModeSticky|0o777))
			}
		}
	})
	home := filepath.Dir(filepath.Dir(p)) // <home>/containers/<name>
	if c["network"] != "none" {
		name := filepath.Base(p)
		members := []obj{}
		if n, ok := c["net_name"].(string); ok && n != "bridge" {
			members = membersOn(home, n)
		}
		networkFiles(r, hostsFile(home, name, c, members))
		args = append(args, "--outbound")
	}
	args = append(args, "--hostname", hostname(filepath.Base(p), c))
	if ip, ok := c["ip"].(string); ok && c["network"] != "none" {
		args = append(args, "--network", networkPorts(home, str(c["net_name"])), "--ip", ip)
	}
	// <home>/containers/<name>: volumes are in <home>/volumes.
	args = append(args, prepareMounts(filepath.Dir(filepath.Dir(p)), r, c, publish)...)
	if c["arch"] == "amd64" { // FEX runs its x86 programs (x86.go)
		args = append(args, x86Args(r, native, home)...)
	}
	// dockiva-engine --tmp memory: /tmp (unless mounted) and /dev/shm in the runtime's
	// memory (memfs), as tmpfs mounts; Docker keeps /tmp on the writable layer. Not for
	// a build's RUN steps: their /tmp is part of the image (a later step reads it).
	if os.Getenv("DOCKIVA_TMP") == "memory" && publish && !strings.HasPrefix(filepath.Base(p), "dockiva-build-") {
		if !hasMount(c, "/tmp") {
			_ = safeRun(func() { r.mountPoint("tmp", true) })
			args = append(args, "--tmpfs", "/tmp:mode=1777")
		}
		if !hasMount(c, "/dev/shm") {
			args = append(args, "--tmpfs", "/dev/shm:mode=1777,size=64m,noexec,nosuid,nodev")
		}
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
	for _, v := range env {
		args = append(args, "--env", v)
	}
	args = append(args, userArgs(r, c, env)...)
	args = append(args, extra...)
	args = append(args, "--")
	args = append(args, command[1:]...)
	if c["host_sandbox"] == true {
		return sandboxArgs(native, args, r, c["writable"] == true)
	}
	return append([]string{native}, args...)
}

// processName runs a container as a macOS process named after it, so Activity
// Monitor, top and the Energy tab show each container by name, with its own CPU,
// memory and energy: a hard link to the runtime (the same signed file) in
// <home>/process, or the runtime itself where no link can be made.
// macOS checks each new executable path once (syspolicyd's malware scan, an online
// lookup of about 280 ms), so links outlive their containers: a container created
// again under the same name (docker compose up) starts at once. Names the engine
// made up (docker run without --name) never come back, so they run the runtime.
func processName(p, runtime string) string {
	if generatedName.MatchString(filepath.Base(p)) {
		return runtime
	}
	link := filepath.Join(filepath.Dir(filepath.Dir(p)), "process", filepath.Base(p))
	if have, e := os.Stat(link); e == nil {
		if target, e := os.Stat(runtime); e == nil && os.SameFile(have, target) {
			return link
		}
		_ = os.Remove(link) // an older runtime
	}
	if os.MkdirAll(filepath.Dir(link), 0o700) != nil || os.Link(runtime, link) != nil {
		return runtime
	}
	return link
}

// generatedName: the names create makes up (token()[:24]).
var generatedName = regexp.MustCompile(`^[0-9a-f]{24}$`)

// pruneProcessNames drops process name links to runtimes other than these (an
// earlier engine's, which they would keep on disk).
func pruneProcessNames(home string, runtimes ...string) {
	rows, _ := os.ReadDir(filepath.Join(home, "process"))
	for _, r := range rows {
		link := filepath.Join(home, "process", r.Name())
		have, e := os.Lstat(link)
		current := false
		for _, runtime := range runtimes {
			target, err := os.Stat(runtime)
			current = current || e == nil && err == nil && os.SameFile(have, target)
		}
		if !current {
			_ = os.Remove(link)
		}
	}
}

func supervise(p, native, worker, generation string) (result error) {
	lock := os.NewFile(3, "container-lock")
	defer lock.Close()
	st, err := lock.Stat()
	check(err)
	reject(st.Mode().IsRegular(), "Supervisor requires an inherited container lock")
	check(syscall.Flock(3, syscall.LOCK_EX|syscall.LOCK_NB))
	status := obj{"state": "starting", "generation": generation, "supervisor": "dockivad", "supervisor_pid": os.Getpid()}
	// start waits on fd 4: closed once the first run's status is saved, or after the
	// failed status below, so it reads status.json when there is something to read.
	var ready *os.File
	if os.Getenv("DOCKIVA_READY_FD") == "4" {
		syscall.CloseOnExec(4) // the run must not hold it
		ready = os.NewFile(4, "ready")
	}
	defer func() {
		if ready != nil {
			ready.Close()
		}
	}()
	// And fd 5 once the last status is saved, after letting go of the container's
	// lock: the engine may remove or start the container again while this process
	// (and, after an exit report, the run's guard) is still on its way out.
	var over *os.File
	if os.Getenv("DOCKIVA_OVER_FD") == "5" {
		syscall.CloseOnExec(5)
		over = os.NewFile(5, "over")
	}
	var listener net.Listener
	var cmd *exec.Cmd
	var writer *os.File
	var done chan error
	finished := false
	final := durable // how the last status is saved
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
		if e := safeRun(func() { store(filepath.Join(p, "status.json"), status, final) }); e != nil {
			result = e
		}
		if over != nil {
			_ = syscall.Flock(3, syscall.LOCK_UN) // also the guard's copy: one open file
			over.Close()
		}
	}()
	c := readObject(filepath.Join(p, "config.json"))
	if c["auto_remove"] == true {
		// docker run --rm: the container is removed right after this status, which
		// only has to be whole. A power loss before the removal can leave it behind,
		// stopped (as a --rm container can be after a Docker daemon crash).
		final = whole
	}
	learnProbes(os.Getenv("DOCKIVA_PROBES"))
	command := stringsOf(c["command"])
	env := stringsOf(c["env"])
	clearPorts(filepath.Dir(filepath.Dir(p)), c) // left by a run that did not end cleanly
	extra := []string{}
	if c["tty"] == true || c["open_stdin"] == true {
		extra = append(extra, "--stdin")
	}
	published, closePorts := publishPorts(p, c, os.Getenv("DOCKIVA_DEFAULT_BIND_IP")) // held across restarts
	defer closePorts()
	extra = append(extra, published...)
	invocation := runtimeCommand(p, native, c, command, command[0], env, true, extra...)
	invocation[0] = processName(p, invocation[0])
	stop := stopSignalNumber(c)
	listenerErr := error(nil)
	listener, listenerErr = net.Listen("tcp", "127.0.0.1:0")
	check(listenerErr)
	secret := token()
	// Read only while this supervisor holds the lock (status turns a lost one into failed).
	store(filepath.Join(p, "control.json"), obj{"port": listener.Addr().(*net.TCPAddr).Port, "token": secret}, volatile)
	out := openStampedLog(p, "stdout") // through pipes: each line's time is indexed
	defer out.Close()
	errout := openStampedLog(p, "stderr")
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
	reported := -1 // the run's exit status from the runtime's exit report; -1: from its guard
	var started time.Time
	// launchWorker starts the run's guard, which starts the runtime.
	launchWorker := func() {
		reader, w, e := os.Pipe() // the worker's liveness pipe: it ends the run if we go
		check(e)
		writer = w
		defer reader.Close()
		cmd = exec.Command(worker, invocation...)
		cmd.ExtraFiles = []*os.File{reader}
		cmd.Stdout = out
		cmd.Stderr = errout
		cmd.WaitDelay = time.Second // output a leftover process holds does not delay the run's end
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
	}
	// launch starts a run: the first, or a restart under the container's policy.
	launch := func() {
		finished = false
		_ = os.Remove(filepath.Join(p, "oom"))    // OOMKilled describes the latest run
		_ = os.Remove(filepath.Join(p, "paused")) // left by a run that ended frozen
		reported = -1
		if guard, liveness, drained, report := prestartedRun(invocation, c, out, errout); guard != nil {
			// A spare supervisor's first run: its guard and runtime are already started.
			// The run is over at the runtime's exit report, or else at its guard's exit;
			// after a report, the guard and runtime are reaped in the background.
			cmd, writer = guard, liveness
			copied = make(chan struct{})
			close(copied)
			done = make(chan error, 1)
			waited := make(chan error, 1)
			go func(cmd *exec.Cmd) { waited <- cmd.Wait() }(cmd)
			go func(done chan error) {
				select {
				case code, ok := <-report:
					if ok {
						drained()
						reported = code
						done <- nil
						return
					}
					e := <-waited
					drained()
					done <- e
				case e := <-waited:
					drained()
					done <- e
				}
			}(done)
		} else {
			launchWorker()
		}
		started = time.Now()
		status["state"] = "running"
		status["pid"] = cmd.Process.Pid
		status["started_at"] = now()
		// Lost in a crash, the run reads as its previous state; status() also
		// reports a running one without its supervisor as failed.
		store(filepath.Join(p, "status.json"), status, whole)
		if ready != nil {
			ready.Close()
			ready = nil
		}
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
		// Also cover an externally killed guard: no worker may outlive its run (after
		// an exit report, the runtime is only tearing itself down).
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		code := reported
		if code < 0 {
			code = cmd.ProcessState.ExitCode()
		}
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
		_ = os.Remove(filepath.Join(p, "paused"))
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
			upperSync(p) // the files the run keeps in memory, before nothing can write them
			_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
			<-done
		}
		finish()
	}
	launch()
	restarts, delay := 0, time.Duration(0)
	var restartAt <-chan time.Time
	// Control connections arrive on a channel, so the loop below waits for them, the
	// run's end and signals at once (it noticed an exit only between 100 ms accepts).
	conns := make(chan net.Conn)
	accepted := listener
	go func() {
		for {
			conn, e := accepted.Accept()
			if e != nil {
				close(conns) // the listener closed: the supervisor is ending
				return
			}
			conns <- conn
		}
	}()
	for {
		var conn net.Conn
		select {
		case <-done:
			code := finish()
			_ = safeRun(func() { c["restart"] = readObject(filepath.Join(p, "config.json"))["restart"] }) // docker update --restart
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
		case next, ok := <-conns:
			reject(ok, "Supervisor control listener closed")
			conn = next
		}
		if conn == nil {
			continue
		}
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
			if allowed && syscall.Signal(n) == syscall.SIGKILL {
				upperSync(p) // docker kill: the run's files in memory first
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
func networkFiles(r layers, hosts string) {
	host, _ := os.ReadFile("/etc/resolv.conf") // macOS keeps it current; missing means fallback
	r.writeFile("etc/resolv.conf", resolvConf(host))
	r.writeFile("etc/hosts", hosts)
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

// upperSync has a container's run write the files it keeps in memory (runtime
// --upper-memory) to the writable layer, before something reads the layer's
// directory itself: docker cp, export, diff and commit, an exec or healthcheck run (a
// runtime of its own, on the same directory), a SIGKILL. The run removes the request
// file once done; a run that ended meanwhile cannot, so the wait is bounded.
func upperSync(p string) {
	if !exists(filepath.Join(p, "upper-memory")) {
		return
	}
	request := filepath.Join(p, "upper-sync")
	f, e := os.OpenFile(request, os.O_CREATE|os.O_WRONLY, 0o600)
	if e != nil {
		return
	}
	f.Close()
	for deadline := time.Now().Add(3 * time.Second); time.Now().Before(deadline); time.Sleep(2 * time.Millisecond) {
		if !exists(request) {
			return
		}
	}
	_ = os.Remove(request)
}

// upperSync for an API request: only a running container's run holds files in memory.
func (b *engine) upperSync(p string) {
	if live(b.status(p)) {
		upperSync(p)
	}
}

// writeGuestFile replaces a regular file (or a symlink, never followed) inside a
// root whose parent directories contain no symlinks.
func writeGuestFile(root, relative, content string) { layers{upper: root}.writeFile(relative, content) }
