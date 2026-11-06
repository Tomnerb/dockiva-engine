package main

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// docker rename keeps the container's ID and host name; a running container must be
// stopped first. docker update changes -m, --cpus and --restart.
func TestRenameAndUpdate(t *testing.T) {
	b := testEngine(t)
	h := b.socketHandler()
	for _, name := range []string{"old", "other"} {
		if r := request(t, h, "POST", "/containers/create?name="+name, `{"Image":"dockiva/shell:demo"}`, ""); r.Code != 201 {
			t.Fatalf("create: %d %s", r.Code, r.Body.String())
		}
	}
	for _, step := range []struct {
		path string
		code int
	}{{"/containers/old/rename?name=new", 204}, {"/containers/new/rename?name=other", 409}, {"/containers/new/rename?name=new", 409}, {"/containers/new/rename?name=bad%20name", 400}} {
		if r := request(t, h, "POST", step.path, "", ""); r.Code != step.code {
			t.Fatalf("%s: %d %s", step.path, r.Code, r.Body.String())
		}
	}
	item := b.inspect("new")
	if item["Id"] != key("old") || item["Name"] != "/new" || object(item["Config"])["Hostname"] != "old" || exists(b.container("old")) {
		t.Fatalf("renamed: %v %v %v", item["Id"], item["Name"], object(item["Config"])["Hostname"])
	}
	if r := request(t, h, "GET", "/containers/"+key("old")[:12]+"/json", "", ""); r.Code != 200 || decodeBody(t, r.Body.String())["Name"] != "/new" {
		t.Fatalf("by ID after rename: %d %s", r.Code, r.Body.String())
	}
	// A running container (its supervisor holds the lock) keeps its name.
	p := b.container("other")
	lock, e := lockFile(filepath.Join(p, "lock"), true)
	check(e)
	save(filepath.Join(p, "status.json"), obj{"state": "running", "generation": "g", "pid": os.Getpid()})
	if r := request(t, h, "POST", "/containers/other/rename?name=moved", "", ""); r.Code != 409 {
		t.Fatalf("rename of a running container: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "POST", "/containers/other/update", `{"Memory":536870912}`, ""); r.Code != 200 || len(decodeBody(t, r.Body.String())["Warnings"].([]any)) != 1 {
		t.Fatalf("update of a running container: %d %s", r.Code, r.Body.String())
	}
	lock.Close()

	// What the docker CLI sends: every field, zero unless set.
	full := `{"BlkioWeight":0,"CpuShares":0,"Memory":536870912,"NanoCpus":2000000000,"MemorySwap":0,"PidsLimit":null,"RestartPolicy":{"Name":"on-failure","MaximumRetryCount":3}}`
	if r := request(t, h, "POST", "/containers/new/update", full, ""); r.Code != 200 || r.Body.String() != `{"Warnings":[]}` {
		t.Fatalf("update: %d %s", r.Code, r.Body.String())
	}
	verify := func() {
		t.Helper()
		host := object(b.inspect("new")["HostConfig"])
		policy := object(host["RestartPolicy"])
		if host["Memory"] != int64(512<<20) || host["NanoCpus"] != int64(2e9) || policy["Name"] != "on-failure" || fmt.Sprint(policy["MaximumRetryCount"]) != "3" {
			t.Fatalf("after update: %v %v %v", host["Memory"], host["NanoCpus"], policy)
		}
	}
	verify()
	if r := request(t, h, "POST", "/containers/new/update", `{"Memory":0,"RestartPolicy":{"Name":"","MaximumRetryCount":0}}`, ""); r.Code != 200 {
		t.Fatalf("empty update: %d %s", r.Code, r.Body.String())
	}
	verify() // nothing asked, nothing changed
	for body, code := range map[string]int{`{"CpuShares":512}`: 501, `{"RestartPolicy":{"Name":"sometimes"}}`: 400, `{"Memory":1024}`: 400} {
		if r := request(t, h, "POST", "/containers/new/update", body, ""); r.Code != code {
			t.Fatalf("%s: %d %s", body, r.Code, r.Body.String())
		}
	}
	if r := request(t, h, "POST", "/containers/new/update", `{"RestartPolicy":{"Name":"no"}}`, ""); r.Code != 200 || object(object(b.inspect("new")["HostConfig"])["RestartPolicy"])["Name"] != "no" {
		t.Fatalf("restart no: %d %s", r.Code, r.Body.String())
	}
}

// docker wait's conditions: not-running (the default), next-exit and removed.
func TestWaitConditions(t *testing.T) {
	b := testEngine(t)
	h := b.socketHandler()
	if r := request(t, h, "POST", "/containers/create?name=w", `{"Image":"dockiva/shell:demo"}`, ""); r.Code != 201 {
		t.Fatalf("create: %d", r.Code)
	}
	if r := request(t, h, "POST", "/containers/w/wait", "", ""); r.Body.String() != "{\"Error\":null,\"StatusCode\":0}\n" {
		t.Fatalf("not-running: %s", r.Body.String())
	}
	if r := request(t, h, "POST", "/containers/w/wait?condition=sometime", "", ""); r.Code != 400 {
		t.Fatalf("bad condition: %d", r.Code)
	}
	waiting := func(condition string) chan string {
		out := make(chan string, 1)
		go func() { out <- request(t, h, "POST", "/containers/w/wait?condition="+condition, "", "").Body.String() }()
		time.Sleep(100 * time.Millisecond)
		select {
		case got := <-out:
			t.Fatalf("%s returned at once: %s", condition, got)
		default:
		}
		return out
	}
	next := waiting("next-exit")
	save(filepath.Join(b.container("w"), "status.json"), obj{"state": "exited", "generation": "g2", "exit_code": 3})
	if got := <-next; got != "{\"Error\":null,\"StatusCode\":3}\n" {
		t.Fatalf("next-exit: %s", got)
	}
	removed := waiting("removed")
	b.remove("w")
	if got := <-removed; got != "{\"Error\":null,\"StatusCode\":3}\n" {
		t.Fatalf("removed: %s", got)
	}
}

// docker stats: PIDs from the run's process table (threads), block I/O from the
// host processes' disk counters.
func TestStatsSample(t *testing.T) {
	table := []byte("1 0 0 3 sh\x00\n7 1 0 4 top\x00\n=boot 1 2 3 4 0.00 0.00 0.00 5\n=1 0 0 0 100 10 1 S\n=7 0 0 0 100 10 3 R\n")
	if n := guestThreads(table); n != 4 {
		t.Fatalf("threads: %d", n)
	}
	if read, wrote := diskIO(int64(os.Getpid())); read < 0 || wrote < 0 {
		t.Fatalf("disk I/O: %d %d", read, wrote)
	}
	sample := statsJSON(key("s"), "s", obj{}, usage{pids: 4, read: 10, wrote: 20, at: time.Now()}, usage{at: time.Now()})
	blkio := object(sample["blkio_stats"])["io_service_bytes_recursive"].([]any)
	if object(sample["pids_stats"])["current"] != int64(4) || blkio[0].(obj)["value"] != int64(10) || blkio[1].(obj)["op"] != "write" {
		t.Fatalf("sample: %v", sample)
	}
}

// A container's macOS process name is a hard link to the runtime in <home>/process,
// kept after the container goes (each new executable path costs macOS an online
// malware-scan lookup on its first exec); engine-made names run the runtime itself.
func TestProcessNameLinks(t *testing.T) {
	home := t.TempDir()
	runtime, other := filepath.Join(home, "runtime"), filepath.Join(home, "older")
	check(os.WriteFile(runtime, []byte("runtime"), 0o700))
	check(os.WriteFile(other, []byte("older"), 0o700))
	p := filepath.Join(home, "containers", "web")
	check(os.MkdirAll(p, 0o700))
	link := processName(p, runtime)
	if link != filepath.Join(home, "process", "web") {
		t.Fatalf("named container: %s", link)
	}
	check(os.RemoveAll(p)) // docker rm, then docker compose up creates it again
	if again := processName(p, runtime); again != link || !exists(link) {
		t.Fatalf("link after removal: %s", again)
	}
	if name := processName(filepath.Join(home, "containers", token()[:24]), runtime); name != runtime {
		t.Fatalf("generated name: %s", name)
	}
	check(os.Link(other, filepath.Join(home, "process", "stale")))
	pruneProcessNames(home, runtime)
	if !exists(link) || exists(filepath.Join(home, "process", "stale")) {
		t.Fatal("prune keeps current links and drops an older runtime's")
	}
}

// A create syncing without the engine lock waits in engine.creating: its name,
// address, network, image and volume stay taken as a container's on disk would.
func TestCreatingHoldsItsClaims(t *testing.T) {
	b := testEngine(t)
	h := b.socketHandler()
	for path, body := range map[string]string{"/networks/create": `{"Name":"n"}`, "/volumes/create": `{"Name":"v"}`} {
		if r := request(t, h, "POST", path, body, ""); r.Code != 201 {
			t.Fatalf("%s: %d %s", path, r.Code, r.Body.String())
		}
	}
	ip := b.assignIP("n")
	b.creating = map[string]obj{"pending": {"image": b.image("dockiva/shell:demo").Digest, "net_name": "n", "ip": ip,
		"mounts": []any{obj{"type": "volume", "source": "v", "target": "/data"}}}}
	if b.assignIP("n") == ip {
		t.Fatal("an address under way given again")
	}
	for _, step := range []struct {
		method, path, body string
		code               int
	}{
		{"DELETE", "/networks/n", "", 409},
		{"DELETE", "/volumes/v", "", 409},
		{"DELETE", "/images/dockiva/shell:demo", "", 409},
		{"POST", "/networks/prune", "", 200},
		{"POST", "/volumes/prune?filters=%7B%22all%22%3A%5B%22true%22%5D%7D", "", 200},
		{"POST", "/images/prune?filters=%7B%22dangling%22%3A%5B%22false%22%5D%7D", "", 200},
		{"POST", "/containers/create?name=pending", `{"Image":"dockiva/shell:demo"}`, 409},
		{"POST", "/containers/create?name=kept", `{"Image":"dockiva/shell:demo"}`, 201},
		{"POST", "/containers/kept/rename?name=pending", "", 409},
	} {
		if r := request(t, h, step.method, step.path, step.body, ""); r.Code != step.code {
			t.Fatalf("%s %s: %d %s", step.method, step.path, r.Code, r.Body.String())
		}
	}
	if !exists(filepath.Join(networksDir(b.Home), "n")) || !exists(filepath.Join(volumesDir(b.Home), "v")) || safeRun(func() { b.image("dockiva/shell:demo") }) != nil {
		t.Fatal("pruned what a create under way uses")
	}
	if len(b.creating) != 1 {
		t.Fatalf("a finished create left its claims: %v", b.creating)
	}
}
