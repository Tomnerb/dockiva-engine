package main

import (
	"context"
	"flag"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"runtime/debug"
	"strings"
	"sync"
	"syscall"
	"time"
)

func cleanupUnpublished(state string) {
	entries, err := os.ReadDir(state)
	check(err)
	for _, entry := range entries {
		if entry.IsDir() && (strings.HasPrefix(entry.Name(), ".creating-") || strings.HasPrefix(entry.Name(), ".removing-")) {
			check(removeTree(filepath.Join(state, entry.Name())))
		}
	}
}

func run() {
	syscall.Umask(0077)
	if len(os.Args) > 1 && os.Args[1] == "_supervise" {
		reject(len(os.Args) == 6, "Invalid supervisor arguments")
		check(supervise(os.Args[2], os.Args[3], os.Args[4], os.Args[5]))
		return
	}
	if len(os.Args) == 4 && os.Args[1] == "_supervise_spare" {
		check(superviseSpare(os.Args[2], os.Args[3]))
		return
	}
	self, e := os.Executable()
	check(e)
	self, e = filepath.EvalSymlinks(self)
	check(e)
	base := filepath.Dir(self)
	assets := filepath.Join(base, "_internal")
	if !exists(assets) {
		assets, e = filepath.Abs(".")
		check(e)
	}
	homeDefault, e := os.UserHomeDir()
	check(e)
	flags := flag.NewFlagSet("dockivad", flag.ContinueOnError)
	home := flags.String("home", filepath.Join(homeDefault, ".dockiva-engine"), "private state")
	native := flags.String("native", filepath.Join(assets, "build/dockiva-runtime"), "C interpreter")
	worker := flags.String("worker", filepath.Join(assets, "build/dockiva-worker"), "C worker guard")
	assetRoot := flags.String("assets", assets, "OCI fixtures")
	tcp := flags.Int("tcp", -1, "loopback API port (0 selects a free port)")
	importName := flags.String("import-name", "", "import name then exit")
	layout := flags.String("import-layout", "", "local OCI layout")
	pull := flags.String("pull", "", "registry reference then exit")
	insecure := flags.Bool("insecure-local", false, "allow plain HTTP to loopback registries (127.0.0.1, localhost)")
	tokenEnv := flags.String("token-env", "", "registry token environment variable")
	maxContainers := flags.Int("max-containers", 64, "running containers at once")
	bindIP := flags.String("default-bind-ip", "127.0.0.1", "host address of -p HOST:CONTAINER ports without one (Docker's --ip; 0.0.0.0 publishes on every address, as Docker does)")
	tmp := flags.String("tmp", "disk", "containers' /tmp and /dev/shm: disk (the container's files, as Docker) or memory (a tmpfs the runtime keeps in memory, counted against the memory limit: no host file system calls, several times faster for many small files; not shared with docker exec)")
	rootfs := flags.String("rootfs", "layered", "containers' files: layered (each container's own changes over its image's files, which all its containers share, as Docker's overlay2: they stay warm in the file cache) or copy (each container gets an APFS clone of its image's files)")
	fsync := flags.String("fsync", "barrier", "containers' fsync: barrier (ordered writes, OrbStack-like), full (drains the drive cache; strictly durable, slower) or fast (plain macOS fsync)")
	minFreeMB := flags.Uint64("min-free-mb", minFree>>20, "the Mac's free space (MB) under which containers on the store are paused and starts, pulls and builds refused: a full disk would eject the store")
	check(flags.Parse(os.Args[1:]))
	minFree = *minFreeMB << 20
	reject(flags.NArg() == 0, "Unexpected arguments")
	reject(*fsync == "barrier" || *fsync == "full" || *fsync == "fast", "--fsync must be barrier, full or fast")
	check(os.Setenv("DOCKIVA_FSYNC", *fsync)) // read by runtimeCommand here and in supervisors, which inherit it
	reject(*tmp == "disk" || *tmp == "memory", "--tmp must be disk or memory")
	check(os.Setenv("DOCKIVA_TMP", *tmp))
	reject(*rootfs == "layered" || *rootfs == "copy", "--rootfs must be layered or copy")
	check(os.Setenv("DOCKIVA_ROOTFS", *rootfs))
	reject(*maxContainers >= 1 && *maxContainers <= 1024, "--max-containers must be 1..1024")
	reject(net.ParseIP(*bindIP) != nil, "--default-bind-ip must be an IP address")
	b := &engine{Home: private(*home), Self: self, Assets: *assetRoot, MaxContainers: *maxContainers, Insecure: *insecure, BindIP: *bindIP, quit: make(chan struct{}, 1)}
	lock, e := lockFile(filepath.Join(b.Home, "daemon.lock"), true)
	check(e)
	defer lock.Close()
	defer detachStore(b.Home, 1)
	// The start's file work: the store attached (store.go: the case-sensitive volume)
	// and, once, an older home moved into it, which copies its images and containers
	// (about 15 s a GB).
	files := func() {
		store := openStore(b.Home)
		b.State = private(filepath.Join(store, "containers"))
		b.Cache = private(filepath.Join(store, "images"))
		if store != b.Home && (exists(filepath.Join(b.Home, "images")) || exists(filepath.Join(b.Home, "containers"))) {
			why := "Dockiva Engine is moving its images and containers to a case-sensitive disk image, once (about 15 s a GB); try again in a minute"
			b.opening.Store(&why)
			moveToStore(b.Home, store)
		}
		cleanupUnpublished(b.State)
		cleanupUnpublished(b.Cache)
		migrateNames(b.Cache)
	}
	if *importName != "" {
		reject(*layout != "", "Import requires --import-layout")
		files()
		fmt.Println(commitImage(b.Cache, *importName, *layout))
		return
	}
	if *pull != "" {
		files()
		fmt.Println(pullWith(*pull, b.Cache, *insecure, credentials{RegistryToken: os.Getenv(*tokenEnv)}, b.commitPulled))
		return
	}
	b.Native = regularExecutable(*native)
	b.Worker = regularExecutable(*worker)
	pruneProcessNames(b.Home, b.Native, filepath.Join(filepath.Dir(b.Native), "dockiva-hvf"))
	profileCatalogParsed() // checked at start: a bad catalog stops the engine
	secret := token()
	endpoint := filepath.Join(b.Home, "engine.sock")
	// docker.sock serves the same API without the token, for the docker CLI and
	// Docker tools: the private (0700) engine home and the 0600 socket are the
	// access control, as with Docker's own socket.
	userSocket := filepath.Join(b.Home, "docker.sock")
	unixListener := func(path string) net.Listener {
		if exists(path) {
			st, e := os.Lstat(path)
			check(e)
			reject(st.Mode()&os.ModeSocket != 0, "Refusing to replace a non-socket endpoint")
			check(os.Remove(path))
		}
		l, e := net.Listen("unix", path)
		check(e)
		check(os.Chmod(path, 0600))
		return l
	}
	var listener, userListener net.Listener
	var host string
	if *tcp < 0 {
		listener = unixListener(endpoint)
		host = "unix://" + endpoint
		defer os.Remove(endpoint)
		userListener = unixListener(userSocket)
		defer os.Remove(userSocket)
		defer userListener.Close()
		if *home == filepath.Join(homeDefault, ".dockiva-engine") || *home == filepath.Join(homeDefault, ".vmfree") { // not for test or secondary engine homes
			if e := safeRun(func() { registerDockerContext(userSocket) }); e != nil {
				fmt.Fprintln(os.Stderr, "dockivad: docker context:", e)
			}
		}
	} else {
		reject(*tcp <= 65535, "Invalid API port")
		listener, e = net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *tcp))
		check(e)
		host = "tcp://" + listener.Addr().String()
	}
	defer listener.Close()
	configdir := private(filepath.Join(b.Home, "docker-client"))
	save(filepath.Join(configdir, "config.json"), obj{"HttpHeaders": obj{"X-VMFree-Token": secret}})
	connection := obj{"host": host, "config": configdir, "api_version": "1.45", "engine": "dockivad", "state_format": 1}
	if userListener != nil {
		connection["docker_host"] = "unix://" + userSocket
	}
	save(filepath.Join(b.Home, "connection.json"), connection)
	fmt.Println("API " + host)
	server := &http.Server{Handler: b.handler(secret), ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 40 * time.Second, IdleTimeout: 10 * time.Second, MaxHeaderBytes: 16384}
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, syscall.SIGTERM, syscall.SIGINT)
	defer signal.Stop(signals)
	done := make(chan struct{})
	defer close(done)
	served := make(chan error, 1)
	var docker *http.Server
	if userListener != nil {
		docker = &http.Server{Handler: b.socketHandler(), ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 40 * time.Second, IdleTimeout: 10 * time.Second, MaxHeaderBytes: 16384}
		defer docker.Close()
	}
	var serving sync.Once
	serve := func() {
		serving.Do(func() {
			go func() { served <- server.Serve(listener) }()
			if docker != nil {
				go func() { _ = docker.Serve(userListener) }()
			}
		})
	}
	// The app waits 15 s for /_ping, then kills the engine. The store attaches in half a
	// second, but has taken macOS minutes (store.go), and a move into it takes about 15 s
	// a GB: after 3 s the API answers /_ping, and asks for patience otherwise, until the
	// files are ready. A start that fails sooner exits before answering.
	why := "Dockiva Engine is starting (macOS can take minutes to attach its disk image); try again in a minute"
	b.opening.Store(&why)
	early := time.AfterFunc(3*time.Second, serve)
	go func() {
		// SIGTERM leaves containers running; POST /dockiva/shutdown has stopped them.
		select {
		case <-signals:
		case <-b.quit:
		case <-done:
			return
		}
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()
		_ = server.Shutdown(ctx)
	}()
	files()
	for _, seed := range [][2]string{{"dockiva/shell:demo", "examples/m19/shell-image"}, {"dockiva/server:demo", "examples/m11_17/http-image"}, {"dockiva/redis:preview", "examples/redis-preview/image"}} {
		source := filepath.Join(b.Assets, seed[1])
		if exists(source) && !exists(filepath.Join(b.Cache, key(seed[0]))) {
			commitImage(b.Cache, seed[0], source)
		}
	}
	// OCI validation has a large, short-lived working set. Return it to the host
	// before idle service, rather than waiting for another allocation-triggered GC.
	debug.FreeOSMemory()
	early.Stop()
	b.opening.Store(nil)
	serve()
	go b.watch(done)
	if b.storeDir() != b.Home {
		go b.guardSpace(done) // store.go: a full Mac would eject the store
	}
	b.spares.on = true // after the Setenvs above: a spare inherits them as an exec'd supervisor would
	b.refillSpare()
	go b.restartOnBoot()
	if e = <-served; e != http.ErrServerClosed {
		check(e)
	}
}
func main() {
	if e := safeRun(run); e != nil {
		fmt.Fprintln(os.Stderr, "dockivad:", e)
		os.Exit(125)
	}
}
