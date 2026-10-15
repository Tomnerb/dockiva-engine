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
	"syscall"
	"time"
)

func cleanupUnpublished(state string) {
	entries, err := os.ReadDir(state)
	check(err)
	for _, entry := range entries {
		if entry.IsDir() && (strings.HasPrefix(entry.Name(), ".creating-") || strings.HasPrefix(entry.Name(), ".removing-")) {
			check(os.RemoveAll(filepath.Join(state, entry.Name())))
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
	insecure := flags.Bool("insecure-local", false, "allow literal loopback registry")
	tokenEnv := flags.String("token-env", "", "registry token environment variable")
	maxContainers := flags.Int("max-containers", 64, "running containers at once")
	check(flags.Parse(os.Args[1:]))
	reject(flags.NArg() == 0, "Unexpected arguments")
	reject(*maxContainers >= 1 && *maxContainers <= 1024, "--max-containers must be 1..1024")
	b := &engine{Home: private(*home), Self: self, Assets: *assetRoot, MaxContainers: *maxContainers, quit: make(chan struct{}, 1)}
	b.State = private(filepath.Join(b.Home, "containers"))
	b.Cache = private(filepath.Join(b.Home, "images"))
	lock, e := lockFile(filepath.Join(b.Home, "daemon.lock"), true)
	check(e)
	defer lock.Close()
	cleanupUnpublished(b.State)
	migrateNames(b.Cache)
	if *importName != "" {
		reject(*layout != "", "Import requires --import-layout")
		fmt.Println(commitImage(b.Cache, *importName, *layout))
		return
	}
	if *pull != "" {
		fmt.Println(pullImage(*pull, b.Cache, *insecure, os.Getenv(*tokenEnv)))
		return
	}
	b.Native = regularExecutable(*native)
	b.Worker = regularExecutable(*worker)
	loadProfileCatalog(profileCatalogJSON)
	for _, seed := range [][2]string{{"dockiva/shell:demo", "examples/m19/shell-image"}, {"dockiva/server:demo", "examples/m11_17/http-image"}, {"dockiva/redis:preview", "examples/redis-preview/image"}} {
		source := filepath.Join(b.Assets, seed[1])
		if exists(source) && !exists(filepath.Join(b.Cache, key(seed[0]))) {
			commitImage(b.Cache, seed[0], source)
		}
	}
	// OCI validation has a large, short-lived working set. Return it to the host
	// before idle service, rather than waiting for another allocation-triggered GC.
	debug.FreeOSMemory()
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
	go b.watch(done)
	go b.restartOnBoot()
	if userListener != nil {
		docker := &http.Server{Handler: b.socketHandler(), ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 40 * time.Second, IdleTimeout: 10 * time.Second, MaxHeaderBytes: 16384}
		go func() { _ = docker.Serve(userListener) }()
		defer docker.Close()
	}
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
	e = server.Serve(listener)
	if e != http.ErrServerClosed {
		check(e)
	}
}
func main() {
	if e := safeRun(run); e != nil {
		fmt.Fprintln(os.Stderr, "dockivad:", e)
		os.Exit(125)
	}
}
