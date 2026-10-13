package main

import (
	"crypto/subtle"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"runtime/debug"
	"sort"
	"strconv"
	"strings"
	"time"
)

func (b *engine) imagePath(name string) string {
	p := filepath.Join(b.Cache, key(name))
	if !exists(p) && !strings.Contains(name, "@") && !strings.Contains(name[strings.LastIndex(name, "/")+1:], ":") {
		p = filepath.Join(b.Cache, key(name+":latest")) // Docker treats an untagged name as :latest.
	}
	if !exists(p) {
		panic(apiError{404, "Image is not cached: " + name})
	}
	return p
}
func (b *engine) image(name string) *image {
	return loadImage(noLinks(b.imagePath(name), "layout"))
}
func (b *engine) imageInfo(name string) obj {
	im := b.image(name)
	var size int
	for _, v := range im.Tree {
		size += len(v.Data)
	}
	st, e := os.Stat(filepath.Join(b.imagePath(name), "image.json"))
	check(e)
	return obj{"Id": im.Digest, "RepoTags": []string{name}, "RepoDigests": []string{strings.Split(name, ":")[0] + "@" + im.Digest}, "Created": st.ModTime().UTC().Format(time.RFC3339Nano), "Size": size, "Architecture": "arm64", "Os": "linux", "Config": obj{"Entrypoint": im.Entry, "Cmd": im.Args, "Env": im.Env, "WorkingDir": im.WorkingDir}, "RootFS": obj{"Type": "layers", "Layers": []string{}}}
}
func (b *engine) inspect(name string) obj {
	p := b.container(name)
	c := readObject(filepath.Join(p, "config.json"))
	s := b.status(p)
	cmd := stringsOf(c["command"])
	reject(len(cmd) > 0, "Invalid saved command")
	state := text(s["state"])
	running := live(s)
	if state == "failed" {
		state = "dead"
	}
	img := c["image_name"]
	if img == nil {
		img = c["image"]
	}
	pid := any(0)
	if running && s["pid"] != nil {
		pid = s["pid"]
	}
	code := any(0)
	if s["exit_code"] != nil {
		code = s["exit_code"]
	}
	labels := object(c["labels"])
	bindings, exposed := dockerPorts(c)
	return obj{"Id": key(name), "Name": "/" + name, "Created": timestamp(c["created_at"]), "Path": cmd[0], "Args": cmd[1:], "Image": c["image"], "State": obj{"Status": state, "Running": running, "Paused": false, "Restarting": false, "OOMKilled": false, "Dead": state == "dead", "Pid": pid, "ExitCode": code, "Error": text(s["error"]), "StartedAt": timestamp(s["started_at"]), "FinishedAt": timestamp(s["finished_at"])}, "Config": obj{"Image": img, "Entrypoint": cmd[:1], "Cmd": cmd[1:], "Env": stringsOf(c["env"]), "Labels": labels, "Tty": false, "WorkingDir": workdir(c), "Hostname": name, "ExposedPorts": exposed}, "HostConfig": obj{"NetworkMode": inspectNetwork(c), "PortBindings": bindings, "ReadonlyRootfs": c["writable"] != true, "AutoRemove": c["auto_remove"] == true, "RestartPolicy": obj{"Name": "no", "MaximumRetryCount": 0}, "LogConfig": obj{"Type": "json-file", "Config": obj{}}}, "NetworkSettings": obj{"Ports": bindings, "Networks": obj{}}, "Mounts": []any{}, "Driver": "vmfree-private-copy", "Platform": "linux", "RestartCount": 0, "DockivaCompatibility": profileInspection(c)}
}
func onlyKeys(m obj, allowed ...string) bool {
	for k := range m {
		found := false
		for _, a := range allowed {
			found = found || k == a
		}
		if !found {
			return false
		}
	}
	return true
}

// workdir is the saved working directory; containers created before it existed use "/".
func workdir(c obj) string {
	if wd := text(c["workdir"]); wd != "" {
		return wd
	}
	return "/"
}

// portBindings validates Docker PortBindings, {"80/tcp":[{"HostIp":"","HostPort":"8080"}]},
// as [container, host] pairs. Ports publish on the host loopback only, and each
// needs an explicit host port.
func portBindings(v any) []any {
	m := object(v)
	reject(len(m) <= 32, "Too many published ports")
	out, guests, hosts := []any{}, map[int64]bool{}, map[int64]bool{}
	for k, raw := range m {
		port, proto, _ := strings.Cut(k, "/")
		if proto != "tcp" && proto != "" {
			panic(apiError{501, "Only TCP port publishing supported"})
		}
		guest, e := strconv.ParseInt(port, 10, 32)
		reject(e == nil && guest >= 1 && guest <= 65535, "Invalid container port: "+k)
		bindings := array(raw)
		reject(len(bindings) == 1, "Publish each container port to exactly one host port")
		b := object(bindings[0])
		reject(onlyKeys(b, "HostIp", "HostPort"), "Unsupported port binding option")
		if ip := text(b["HostIp"]); ip != "" && ip != "127.0.0.1" {
			panic(apiError{501, "VM-free publishes ports on 127.0.0.1 only"})
		}
		host, e := strconv.ParseInt(text(b["HostPort"]), 10, 32)
		reject(e == nil && host >= 1 && host <= 65535, "Explicit host port 1..65535 required for "+k)
		reject(!guests[guest] && !hosts[host], "Duplicate published port")
		guests[guest], hosts[host] = true, true
		out = append(out, []any{guest, host})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].([]any)[0].(int64) < out[j].([]any)[0].(int64) })
	return out
}

// publishedPorts reads the saved [container, host] pairs.
func publishedPorts(c obj) [][2]int64 {
	out := [][2]int64{}
	if c["ports"] == nil {
		return out
	}
	for _, v := range array(c["ports"]) {
		pair := array(v)
		reject(len(pair) == 2, "Invalid saved port")
		out = append(out, [2]int64{integer(pair[0]), integer(pair[1])})
	}
	return out
}

// portRows lists an inspected container's published ports as /containers/json rows.
func portRows(item obj) []any {
	rows := []any{}
	for k, v := range item["HostConfig"].(obj)["PortBindings"].(obj) {
		port, _, _ := strings.Cut(k, "/")
		guest, _ := strconv.ParseInt(port, 10, 32)
		host, _ := strconv.ParseInt(v.([]any)[0].(obj)["HostPort"].(string), 10, 32)
		rows = append(rows, obj{"IP": "127.0.0.1", "PrivatePort": guest, "PublicPort": host, "Type": "tcp"})
	}
	sort.Slice(rows, func(i, j int) bool {
		return rows[i].(obj)["PrivatePort"].(int64) < rows[j].(obj)["PrivatePort"].(int64)
	})
	return rows
}

// dockerPorts renders saved ports as Docker's PortBindings and ExposedPorts maps.
func dockerPorts(c obj) (obj, obj) {
	bindings, exposed := obj{}, obj{}
	for _, p := range publishedPorts(c) {
		k := strconv.FormatInt(p[0], 10) + "/tcp"
		bindings[k] = []any{obj{"HostIp": "127.0.0.1", "HostPort": strconv.FormatInt(p[1], 10)}}
		exposed[k] = obj{}
	}
	return bindings, exposed
}
func (b *engine) create(name string, body obj) obj {
	p := b.container(name)
	if exists(p) {
		panic(apiError{409, "Container name already exists"})
	}
	allowed := map[string]bool{}
	for _, k := range []string{"Image", "Cmd", "Entrypoint", "Env", "Labels", "HostConfig", "WorkingDir", "AttachStdout", "AttachStderr", "NetworkingConfig", "ExposedPorts"} {
		allowed[k] = true
	}
	for k, v := range body {
		if idle(v) {
			continue
		}
		switch k {
		case "Tty":
			panic(apiError{501, "Interactive terminals (docker run -t) are not supported by the VM-free engine"})
		case "OpenStdin", "AttachStdin", "StdinOnce":
			panic(apiError{501, "Standard input (docker run -i) is not supported by the VM-free engine"})
		}
		if !allowed[k] {
			panic(apiError{501, "Unsupported container option: " + k})
		}
	}
	// ExposedPorts is metadata in Docker; only PortBindings publishes.
	for k, v := range object(body["ExposedPorts"]) {
		port, proto, _ := strings.Cut(k, "/")
		n, e := strconv.ParseInt(port, 10, 32)
		reject(e == nil && n >= 1 && n <= 65535 && (proto == "tcp" || proto == "") && idle(v), "Only TCP ExposedPorts supported: "+k)
	}
	network := object(body["NetworkingConfig"])
	if !onlyKeys(network, "EndpointsConfig") {
		panic(apiError{501, "Unsupported NetworkingConfig"})
	}
	endpoints := object(network["EndpointsConfig"])
	// The docker CLI names the endpoint after NetworkMode ("default" unless --network).
	if !onlyKeys(endpoints, "host", "bridge", "default") {
		panic(apiError{501, "Only host network supported"})
	}
	for _, name := range []string{"host", "bridge", "default"} {
		for _, v := range object(endpoints[name]) {
			if !idle(v) {
				panic(apiError{501, "Host network options unsupported"})
			}
		}
	}
	host := object(body["HostConfig"])
	ports := []any{}
	for k, v := range host {
		if idle(v) {
			continue
		}
		ok := false
		switch k {
		case "MemorySwappiness":
			ok = v == json.Number("-1")
		case "ConsoleSize":
			a := array(v)
			ok = len(a) == 2 && idle(a[0]) && idle(a[1])
		case "NetworkMode":
			// Guests share the Mac's network (as with host networking); "none" keeps them on loopback.
			ok = v == "host" || v == "default" || v == "bridge" || v == "none"
		case "ReadonlyRootfs", "AutoRemove":
			_, ok = v.(bool)
		case "PortBindings":
			ports, ok = portBindings(v), true
		case "LogConfig":
			m := object(v)
			ok = onlyKeys(m, "Type", "Config") && (idle(m["Type"]) || m["Type"] == "json-file") && idle(m["Config"])
		case "RestartPolicy":
			m := object(v)
			ok = onlyKeys(m, "Name", "MaximumRetryCount") && (idle(m["Name"]) || m["Name"] == "no") && idle(m["MaximumRetryCount"])
		}
		if !ok {
			panic(apiError{501, "Unsupported HostConfig: " + k})
		}
	}
	imageName := str(body["Image"])
	reject(len(imageName) > 0 && len(imageName) <= 512, "Invalid image name")
	im := b.image(imageName)
	if wd := text(body["WorkingDir"]); wd != "" {
		im.WorkingDir = im.ensureDir(wd)
	}
	if body["Entrypoint"] != nil {
		im.Entry = stringsOf(body["Entrypoint"])
		reject(len(im.Entry) > 0, "Empty Entrypoint unsupported")
	}
	var override []string
	if body["Cmd"] != nil {
		override = stringsOf(body["Cmd"])
		reject(len(override) > 0, "Empty Cmd unsupported")
	}
	merged := map[string]string{}
	order := []string{}
	for _, v := range im.Env {
		k, val, _ := strings.Cut(v, "=")
		merged[k] = val
		order = append(order, k)
	}
	for _, v := range stringsOf(body["Env"]) {
		k, val, ok := strings.Cut(v, "=")
		reject(ok && k != "", "Env requires KEY=VALUE")
		if _, exists := merged[k]; !exists {
			order = append(order, k)
		}
		merged[k] = val
	}
	im.Env = []string{}
	for _, k := range order {
		im.Env = append(im.Env, k+"="+merged[k])
	}
	validateEnv(im.Env)
	labels := object(body["Labels"])
	reject(len(labels) <= 64, "Too many labels")
	for k, v := range labels {
		reject(len(k)+len(str(v)) <= 4096, "Label too long")
	}
	command := im.command(override)
	config := obj{"image": im.Digest, "image_name": imageName, "command": command, "env": im.Env, "writable": host["ReadonlyRootfs"] != true, "cpu_backend": "interpreter", "schedule": false, "process_tree": false, "scheduled_network": false, "dynamic": false, "job_control": false, "memory_limit_mib": 256, "host_sandbox": false, "max_steps": 10000000, "labels": labels, "ports": ports, "workdir": im.WorkingDir, "network": networkMode(host), "stop_signal": im.StopSignal, "created_at": now()}
	if host["AutoRemove"] == true {
		config["auto_remove"] = true // docker run --rm: removed once it exits (engine.watch)
	}
	profile := loadProfileCatalog(profileCatalogJSON).selectForHost(im.Digest, b.Native)
	profile.apply(config)
	validateLaunchProfile(config, b.Native)
	stage, err := os.MkdirTemp(b.State, ".creating-")
	check(err)
	committed := false
	defer func() {
		if !committed {
			_ = os.RemoveAll(stage)
		}
	}()
	check(os.Mkdir(filepath.Join(stage, "rootfs"), 0700))
	im.extract(filepath.Join(stage, "rootfs"))
	save(filepath.Join(stage, "config.json"), config)
	save(filepath.Join(stage, "status.json"), obj{"state": "created"})
	check(os.Rename(stage, p))
	committed = true
	return obj{"Id": key(name), "Warnings": []string{"VM-free preview: bounded Linux compatibility; containers share the Mac's network"}}
}

var versionPath = regexp.MustCompile(`^/v([0-9]+)\.([0-9]+)(/.*)$`)
var containerPath = regexp.MustCompile(`^/containers/([^/]+)(?:/(json|start|stop|restart|logs|wait|attach|kill))?$`)

func send(w http.ResponseWriter, status int, v any) {
	var raw []byte
	content := "application/json"
	if b, ok := v.([]byte); ok {
		raw = b
		content = "application/vnd.docker.raw-stream"
	} else if v != nil {
		var e error
		raw, e = json.Marshal(v)
		check(e)
	}
	w.Header().Set("Content-Type", content)
	w.Header().Set("Content-Length", strconv.Itoa(len(raw)))
	w.WriteHeader(status)
	_, _ = w.Write(raw)
}

// handler serves the engine API to clients presenting the engine token.
func (b *engine) handler(secret string) http.Handler {
	return b.api(func(r *http.Request) bool {
		return subtle.ConstantTimeCompare([]byte(r.Header.Get("X-VMFree-Token")), []byte(secret)) == 1
	})
}

// socketHandler serves the same API without a token, for the user-only Docker
// socket (docker.sock in the private engine home): like Docker's own socket, file
// permissions are the access control, so plain docker and Docker tools work.
func (b *engine) socketHandler() http.Handler {
	return b.api(func(*http.Request) bool { return true })
}
func (b *engine) api(authorized func(*http.Request) bool) http.Handler {
	slots := make(chan struct{}, 16)
	streams := make(chan struct{}, 64) // logs -f, attach, wait and events hold a stream slot instead
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("API-Version", "1.45")
		w.Header().Set("Docker-Experimental", "true")
		defer func() {
			if v := recover(); v != nil {
				status := 500
				message := "Host operation failed; inspect engine log"
				if e, ok := v.(apiError); ok {
					status = e.Code
					message = e.Message
				} else {
					fmt.Fprintln(os.Stderr, v)
				}
				send(w, status, obj{"message": message})
			}
		}()
		if !authorized(r) {
			panic(apiError{401, "Authentication required"})
		}
		select {
		case slots <- struct{}{}:
		default:
			panic(apiError{503, "Request capacity exceeded"})
		}
		held := true
		defer func() {
			if held {
				<-slots
			}
		}()
		// stream moves a long-lived request from the request slots to a stream slot.
		stream := func() {
			select {
			case streams <- struct{}{}:
			default:
				panic(apiError{503, "Too many streaming requests"})
			}
			<-slots
			held = false
			unbounded(w)
		}
		endStream := func() { <-streams }
		if len(r.TransferEncoding) > 0 {
			panic(apiError{400, "Chunked requests unsupported"})
		}
		if r.ContentLength > maxJSON {
			panic(apiError{413, "Request exceeds 1 MiB"})
		}
		raw, e := io.ReadAll(io.LimitReader(r.Body, maxJSON+1))
		check(e)
		if int64(len(raw)) > maxJSON {
			panic(apiError{413, "Request exceeds 1 MiB"})
		}
		path := r.URL.Path
		if m := versionPath.FindStringSubmatch(path); m != nil {
			major, e := strconv.Atoi(m[1])
			check(e)
			minor, e := strconv.Atoi(m[2])
			check(e)
			reject(major == 1 && minor >= 24 && minor <= 45, "Supported API range is 1.24–1.45")
			path = m[3]
		}
		q := r.URL.Query()
		method := r.Method
		yes := func(k string) bool { return q.Get(k) == "1" || q.Get(k) == "true" }
		switch {
		case path == "/_ping" && (method == "GET" || method == "HEAD"):
			w.Header().Set("Content-Type", "text/plain")
			w.WriteHeader(200)
			_, _ = w.Write([]byte("OK"))
			return
		case path == "/version" && method == "GET":
			send(w, 200, obj{"Version": "0.18.0", "ApiVersion": "1.45", "MinAPIVersion": "1.24", "Os": "linux", "Arch": "arm64", "GitCommit": "preview", "GoVersion": runtime.Version(), "KernelVersion": "vmfree-user-mode", "Experimental": true, "Components": []any{obj{"Name": "dockivad", "Version": "0.18.0", "Details": obj{"ControlPlane": "Go", "PythonRuntime": "false"}}}})
			return
		case path == "/info" && method == "GET":
			send(w, 200, obj{"ID": "vmfree", "Name": "vmfree", "ServerVersion": "0.18.0", "OSType": "linux", "Architecture": "aarch64", "OperatingSystem": "VM-free preview on " + runtime.GOOS, "Driver": "private-copy", "Containers": len(b.names()), "NCPU": min(runtime.NumCPU(), 64), "MemTotal": 0, "SecurityOptions": []any{}, "Warnings": []string{"Not a production isolation boundary"}})
			return
		case path == "/containers/create" && method == "POST":
			defer debug.FreeOSMemory()
			reject(q.Get("platform") == "" || q.Get("platform") == "linux/arm64", "Only linux/arm64 supported")
			name := q.Get("name")
			if name == "" {
				name = token()[:24]
			}
			body := obj{}
			if len(raw) > 0 {
				body = decode(raw, maxJSON)
			}
			b.Mu.Lock()
			defer b.Mu.Unlock()
			send(w, 201, b.create(name, body))
			return
		case path == "/containers/json" && method == "GET":
			filters := obj{}
			if q.Get("filters") != "" {
				filters = decode([]byte(q.Get("filters")), maxJSON)
			}
			if !onlyKeys(filters, "label", "name", "status", "id") {
				panic(apiError{501, "Unsupported filter"})
			}
			rows := []any{}
			for _, name := range b.names() {
				item := b.inspect(name)
				state := object(item["State"])
				config := object(item["Config"])
				labels := object(config["Labels"])
				if !yes("all") && state["Running"] != true {
					continue
				}
				include := true
				for k, v := range filters {
					values := []string{}
					switch x := v.(type) {
					case []any:
						for _, v := range x {
							values = append(values, str(v))
						}
					case map[string]any:
						for k := range x {
							values = append(values, k)
						}
					default:
						reject(false, "Invalid filter values")
					}
					match := false
					for _, v := range values {
						switch k {
						case "label":
							key, expected, has := strings.Cut(v, "=")
							actual, ok := labels[key]
							include = include && ok && (!has || actual == expected)
							match = true
						case "name":
							match = match || strings.Contains(name, v)
						case "status":
							match = match || state["Status"] == v
						case "id":
							match = match || strings.HasPrefix(str(item["Id"]), v)
						}
					}
					if k != "label" {
						include = include && match
					}
				}
				if include {
					created, e := time.Parse(time.RFC3339Nano, str(item["Created"]))
					check(e)
					cmd := append([]string{str(item["Path"])}, item["Args"].([]string)...)
					rows = append(rows, obj{"Id": item["Id"], "Names": []any{item["Name"]}, "Image": config["Image"], "ImageID": item["Image"], "Command": strings.Join(cmd, " "), "Created": created.Unix(), "State": state["Status"], "Status": state["Status"], "Labels": labels, "Ports": portRows(item), "HostConfig": obj{"NetworkMode": "host"}, "NetworkSettings": obj{"Networks": obj{}}, "Mounts": []any{}})
				}
			}
			send(w, 200, rows)
			return
		case path == "/images/json" && method == "GET":
			defer debug.FreeOSMemory()
			entries, e := os.ReadDir(b.Cache)
			check(e)
			rows := []any{}
			for _, entry := range entries {
				if entry.IsDir() && len(entry.Name()) == 64 {
					p := filepath.Join(b.Cache, entry.Name(), "image.json")
					meta := readObject(p)
					item := b.imageInfo(str(meta["name"]))
					st, e := os.Stat(p)
					check(e)
					item["Created"] = st.ModTime().Unix()
					rows = append(rows, item)
				}
			}
			send(w, 200, rows)
			return
		case strings.HasPrefix(path, "/images/") && strings.HasSuffix(path, "/json") && method == "GET":
			defer debug.FreeOSMemory()
			send(w, 200, b.imageInfo(strings.TrimSuffix(strings.TrimPrefix(path, "/images/"), "/json")))
			return
		case path == "/events" && method == "GET":
			if (q.Get("since") != "" && q.Get("since") != "0") || q.Get("until") != "" {
				panic(apiError{501, "Event history (since/until) unsupported"})
			}
			filters := obj{}
			if q.Get("filters") != "" {
				filters = decode([]byte(q.Get("filters")), maxJSON)
			}
			stream()
			defer endStream()
			b.streamEvents(w, r, filters)
			return
		case path == "/images/create" && method == "POST":
			defer debug.FreeOSMemory()
			reference := q.Get("fromImage")
			if tag := q.Get("tag"); tag != "" && !strings.Contains(reference, "@") {
				reference += ":" + tag
			}
			b.Mu.Lock()
			defer b.Mu.Unlock()
			pullImage(reference, b.Cache, false, "")
			send(w, 200, obj{"status": "Downloaded verified image " + reference})
			return
		}
		if m := containerPath.FindStringSubmatch(path); m != nil {
			name := b.resolve(m[1])
			action := m[2]
			switch {
			case action == "json" && method == "GET":
				send(w, 200, b.inspect(name))
				return
			case (action == "start" || action == "stop" || action == "restart") && method == "POST":
				b.Mu.Lock()
				defer b.Mu.Unlock()
				if action == "stop" || action == "restart" {
					b.stop(name)
				}
				if action == "start" || action == "restart" {
					b.start(name)
				}
				send(w, 204, nil)
				return
			case action == "logs" && method == "GET":
				if (q.Get("since") != "" && q.Get("since") != "0") || (q.Get("until") != "" && q.Get("until") != "0") || yes("timestamps") {
					panic(apiError{501, "Time-filtered or timestamped logs unsupported"})
				}
				p := b.container(name)
				sizes := logSizes(p)
				data := []byte{}
				for i, stream := range []string{"stdout", "stderr"} {
					if !yes(stream) || !exists(filepath.Join(p, logNames[i])) {
						continue
					}
					log := readBounded(p, logNames[i], 16<<20)
					if int64(len(log)) > sizes[i] {
						log = log[:sizes[i]] // follow continues from here
					}
					tail := q.Get("tail")
					if tail != "" && tail != "all" {
						n, e := strconv.Atoi(tail)
						check(e)
						reject(n >= 0, "Invalid tail")
						lines := strings.SplitAfter(string(log), "\n")
						if lines[len(lines)-1] == "" {
							lines = lines[:len(lines)-1]
						}
						if n < len(lines) {
							lines = lines[len(lines)-n:]
						}
						log = []byte(strings.Join(lines, ""))
					}
					data = frame(data, i+1, log)
				}
				if !yes("follow") {
					send(w, 200, data)
					return
				}
				stream()
				defer endStream()
				w.Header().Set("Content-Type", "application/vnd.docker.raw-stream")
				w.WriteHeader(200)
				_, _ = w.Write(data)
				b.followLogs(w, r, p, yes("stdout"), yes("stderr"), sizes)
				return
			case action == "wait" && method == "POST":
				stream()
				defer endStream()
				b.wait(w, r, name, q.Get("condition"))
				return
			case action == "attach" && method == "POST":
				if yes("stdin") {
					panic(apiError{501, "Standard input is not supported by the VM-free engine; use docker attach --no-stdin"})
				}
				reject(yes("stream") && !yes("logs"), "Attach supports stream=1 without logs replay")
				stream()
				defer endStream()
				b.attach(w, r, name, yes("stdout"), yes("stderr"))
				return
			case action == "kill" && method == "POST":
				b.kill(name, signalNumber(q.Get("signal")))
				send(w, 204, nil)
				return
			case action == "" && method == "DELETE":
				b.Mu.Lock()
				defer b.Mu.Unlock()
				if yes("force") {
					b.stop(name)
				}
				b.remove(name)
				send(w, 204, nil)
				return
			}
		}
		panic(apiError{501, "Unsupported endpoint: " + method + " " + path})
	})
}

// networkMode is "none" (host loopback only) or "outbound" (loopback plus the
// Mac's IPv4 network, the Docker default).
func networkMode(host obj) string {
	if host["NetworkMode"] == "none" {
		return "none"
	}
	return "outbound"
}
func inspectNetwork(c obj) string {
	if c["network"] == "none" {
		return "none"
	}
	return "host"
}
