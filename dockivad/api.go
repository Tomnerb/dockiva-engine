package main

import (
	"cmp"
	"crypto/subtle"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"runtime"
	"runtime/debug"
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"
)

func (b *engine) imagePath(name string) string {
	name = shortName(name)
	p := filepath.Join(b.Cache, key(name))
	if !exists(p) && !strings.Contains(name, "@") && !strings.Contains(name[strings.LastIndex(name, "/")+1:], ":") {
		p = filepath.Join(b.Cache, key(name+":latest")) // Docker treats an untagged name as :latest.
	}
	if !exists(p) && imageID.MatchString(name) { // docker run IMAGE-ID, as docker build prints
		entries, e := os.ReadDir(b.Cache)
		check(e)
		for _, entry := range entries {
			meta := filepath.Join(b.Cache, entry.Name(), "image.json")
			if len(entry.Name()) == 64 && exists(meta) && strings.HasPrefix(str(readObject(meta)["digest"]), "sha256:"+strings.TrimPrefix(name, "sha256:")) {
				return filepath.Join(b.Cache, entry.Name())
			}
		}
	}
	if !exists(p) {
		panic(apiError{404, "Image is not cached: " + name})
	}
	return p
}

// image is a cached image with its unpacked filesystem (read only: containers clone it).
func (b *engine) image(name string) *image {
	p := b.imagePath(name)
	ensureUnpacked(p)
	im := loadImage(noLinks(p, "layout"))
	im.Root = noLinks(p, "rootfs")
	return im
}
func (b *engine) imageInfo(name string) obj {
	im := loadImage(noLinks(b.imagePath(name), "layout"))
	size := readObject(filepath.Join(b.imagePath(name), "image.json"))["size"]
	if size == nil {
		size = 0
	}
	st, e := os.Stat(filepath.Join(b.imagePath(name), "image.json"))
	check(e)
	return obj{"Id": im.Digest, "RepoTags": []string{name}, "RepoDigests": []string{strings.Split(name, ":")[0] + "@" + im.Digest}, "Created": st.ModTime().UTC().Format(time.RFC3339Nano), "Size": size, "Architecture": "arm64", "Os": "linux", "Config": obj{"Entrypoint": im.Entry, "Cmd": im.Args, "Env": im.Env, "WorkingDir": im.WorkingDir}, "RootFS": obj{"Type": "layers", "Layers": []string{}}}
}

// imageFilter is docker images NAME (reference: a name or pattern, with or without
// the tag) and dangling (untagged build results, named "<none>:ID").
func imageFilter(name string, filters obj) bool {
	repo := repository(name)
	for k, v := range filters {
		values := []string{}
		for value := range object(v) {
			values = append(values, value)
		}
		ok := false
		for _, value := range values {
			switch k {
			case "dangling":
				ok = ok || (value == "true") == strings.HasPrefix(name, "<none>:")
			case "reference":
				m1, _ := path.Match(value, name)
				m2, _ := path.Match(value, repo)
				ok = ok || m1 || m2
			}
		}
		if !ok {
			return false
		}
	}
	return true
}

// repository is a name without its tag (a registry port is not a tag).
func repository(name string) string {
	if i := strings.LastIndex(name, ":"); i > strings.LastIndex(name, "/") {
		return name[:i]
	}
	return name
}

var imageID = regexp.MustCompile(`^(sha256:)?[0-9a-f]{4,64}$`)

// removeImage is docker rmi: by name, or by ID when every name it has is removed
// (with force if it has several). An image a container was created from needs force.
func (b *engine) removeImage(ref string, force bool) []obj {
	dirs := map[string]string{} // cache directory: name
	var digest string
	if p := filepath.Join(b.Cache, key(ref)); exists(p) || !imageID.MatchString(ref) {
		p = b.imagePath(ref)
		meta := readObject(filepath.Join(p, "image.json"))
		dirs[p], digest = str(meta["name"]), str(meta["digest"])
	}
	entries, e := os.ReadDir(b.Cache)
	check(e)
	tags := 0
	for _, entry := range entries {
		if !entry.IsDir() || len(entry.Name()) != 64 {
			continue
		}
		p := filepath.Join(b.Cache, entry.Name())
		meta := readObject(filepath.Join(p, "image.json"))
		d := str(meta["digest"])
		if digest == "" && strings.HasPrefix(d, "sha256:"+strings.TrimPrefix(ref, "sha256:")) {
			dirs[p] = str(meta["name"])
		}
		if d == digest || digest == "" && dirs[p] != "" {
			tags++
		}
	}
	if len(dirs) == 0 {
		panic(apiError{404, "No such image: " + ref})
	}
	for p := range dirs {
		digest = str(readObject(filepath.Join(p, "image.json"))["digest"])
	}
	reject(len(dirs) == 1 || force, "Image "+ref+" has several names; remove them by name or use --force")
	last := tags == len(dirs)
	if last && !force {
		for _, name := range b.names() {
			if readObject(filepath.Join(b.container(name), "config.json"))["image"] == digest {
				panic(apiError{409, "conflict: unable to remove image " + ref + " (must force) - container " + name + " is using it"})
			}
		}
	}
	out := []obj{}
	for p, name := range dirs {
		check(os.RemoveAll(p))
		out = append(out, obj{"Untagged": name})
	}
	if last {
		out = append(out, obj{"Deleted": digest})
	}
	return out
}

func restartPolicy(c obj) obj {
	r := object(c["restart"])
	if r["name"] == nil {
		return obj{"Name": "no", "MaximumRetryCount": 0}
	}
	return obj{"Name": r["name"], "MaximumRetryCount": cmp.Or(r["max"], any(0))}
}

// psRow is a /containers/json row of an inspected container.
func psRow(item obj) obj {
	state, config := object(item["State"]), object(item["Config"])
	created, e := time.Parse(time.RFC3339Nano, str(item["Created"]))
	check(e)
	cmd := append([]string{str(item["Path"])}, item["Args"].([]string)...)
	return obj{"Id": item["Id"], "Names": []any{item["Name"]}, "Image": config["Image"], "ImageID": item["Image"], "Command": strings.Join(cmd, " "), "Created": created.Unix(), "State": state["Status"], "Status": psStatus(state), "Labels": config["Labels"], "Ports": portRows(item), "HostConfig": obj{"NetworkMode": object(item["HostConfig"])["NetworkMode"]}, "NetworkSettings": obj{"Networks": object(item["NetworkSettings"])["Networks"]}, "Mounts": item["Mounts"]}
}

// psStatus is docker ps's STATUS: the state, with the health while it has one.
func psStatus(state obj) string {
	if h, ok := state["Health"].(obj); ok && h["Status"] != nil && state["Running"] == true {
		return str(state["Status"]) + " (" + str(h["Status"]) + ")"
	}
	return str(state["Status"])
}
func restartCount(s obj) any { return cmp.Or(s["restart_count"], any(0)) }

// hostname is --hostname, else the name when it is a valid host name, else the short
// container ID, as Docker uses (a name may be too long or hold "_" and dots).
func hostname(name string, c obj) string {
	if h := text(c["hostname"]); h != "" {
		return h
	}
	if hostnamePattern.MatchString(name) && !strings.Contains(name, ".") {
		return name
	}
	return key(name)[:12]
}
func extraHostList(c obj) []string {
	out := []string{}
	if c["extra_hosts"] != nil {
		for _, v := range array(c["extra_hosts"]) {
			pair := stringsOf(v)
			out = append(out, pair[0]+":"+pair[1])
		}
	}
	return out
}

// health is State.Health: absent without a healthcheck.
func health(p string, c obj) any {
	if c["healthcheck"] == nil {
		return nil
	}
	if path := filepath.Join(p, "health.json"); exists(path) {
		return readObject(path)
	}
	return obj{"Status": "starting", "FailingStreak": 0, "Log": []any{}}
}
func healthConfig(c obj) any {
	hc := object(c["healthcheck"])
	if c["healthcheck"] == nil {
		return nil
	}
	return obj{"Test": append([]string{"CMD"}, stringsOf(hc["test"])...), "Interval": hc["interval"], "Timeout": hc["timeout"], "StartPeriod": hc["start_period"], "StartInterval": hc["start_interval"], "Retries": hc["retries"]}
}

func nanoCpus(c obj) int64 {
	if c["cpus"] == nil {
		return 0
	}
	return integer(c["cpus"]) * 1e9
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
	binds := []string{}
	if c["binds"] != nil {
		binds = stringsOf(c["binds"])
	}
	return obj{"Id": key(name), "Name": "/" + name, "Created": timestamp(c["created_at"]), "Path": cmd[0], "Args": cmd[1:], "Image": c["image"], "State": obj{"Status": state, "Running": running, "Paused": false, "Restarting": s["state"] == "restarting", "Health": health(p, c), "OOMKilled": exists(filepath.Join(p, "oom")), "Dead": state == "dead", "Pid": pid, "ExitCode": code, "Error": text(s["error"]), "StartedAt": timestamp(s["started_at"]), "FinishedAt": timestamp(s["finished_at"])}, "Config": obj{"Image": img, "Entrypoint": cmd[:1], "Cmd": cmd[1:], "Env": stringsOf(c["env"]), "Labels": labels, "User": text(c["user"]), "Tty": c["tty"] == true, "OpenStdin": c["open_stdin"] == true, "StdinOnce": c["stdin_once"] == true, "WorkingDir": workdir(c), "Hostname": hostname(name, c), "Healthcheck": healthConfig(c), "ExposedPorts": exposed}, "HostConfig": obj{"NetworkMode": inspectNetwork(c), "PortBindings": bindings, "ReadonlyRootfs": c["writable"] != true, "AutoRemove": c["auto_remove"] == true, "Binds": binds, "Memory": memoryMiB(c) << 20, "NanoCpus": nanoCpus(c), "RestartPolicy": restartPolicy(c), "ExtraHosts": extraHostList(c), "LogConfig": obj{"Type": "json-file", "Config": obj{}}}, "NetworkSettings": obj{"Ports": bindings, "Networks": b.containerNetworks(name, c), "IPAddress": text(c["ip"]), "Gateway": gatewayOf(c)}, "Mounts": b.inspectMounts(c), "Driver": "dockiva-private-copy", "Platform": "linux", "RestartCount": restartCount(s), "DockivaCompatibility": profileInspection(c)}
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
			panic(apiError{501, "Dockiva Engine publishes ports on 127.0.0.1 only"})
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
func (b *engine) create(name string, body obj) obj { return b.createWith(name, body, nil) }

// createWith is create from a prepared image (a build step's root and settings)
// instead of the cached image body names.
func (b *engine) createWith(name string, body obj, prepared *image) obj {
	p := b.container(name)
	if exists(p) {
		panic(apiError{409, "Container name already exists"})
	}
	allowed := map[string]bool{}
	for _, k := range []string{"Image", "Cmd", "Entrypoint", "Env", "Labels", "HostConfig", "WorkingDir", "AttachStdout", "AttachStderr", "NetworkingConfig", "ExposedPorts", "Volumes", "User", "Tty", "OpenStdin", "StdinOnce", "AttachStdin", "Healthcheck", "Hostname", "Domainname", "StopTimeout", "StopSignal"} {
		allowed[k] = true
	}
	for k, v := range body {
		if idle(v) {
			continue
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
	if len(endpoints) > 1 {
		panic(apiError{501, "One network per container is supported"})
	}
	aliases := []string{}
	for _, endpoint := range endpoints {
		for k, v := range object(endpoint) {
			switch {
			case k == "Aliases" || k == "DNSNames": // --network-alias; Compose's service names
				for _, a := range stringsOf(v) {
					reject(namePattern.MatchString(a) || hostnamePattern.MatchString(a), "Invalid network alias: "+a)
					if a != name && !slices.Contains(aliases, a) {
						aliases = append(aliases, a)
					}
				}
			case !idle(v):
				panic(apiError{501, "Unsupported network endpoint option: " + k})
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
		case "Memory", "NanoCpus":
			ok = integer(v) > 0 // docker run -m, --cpus
		case "MemorySwap":
			ok = integer(v) >= -1 // there is no swap; the limit is -m
		case "ConsoleSize":
			a := array(v)
			ok = len(a) == 2 && (idle(a[0]) && idle(a[1]) || integer(a[0]) > 0 && integer(a[0]) < 65536 && integer(a[1]) > 0 && integer(a[1]) < 65536)
		case "NetworkMode":
			mode, isText := v.(string)
			ok = isText && !strings.HasPrefix(mode, "container:")
		case "ReadonlyRootfs", "AutoRemove":
			_, ok = v.(bool)
		case "Binds", "Mounts":
			ok = true // parseMounts validates them
		case "PortBindings":
			ports, ok = portBindings(v), true
		case "LogConfig":
			m := object(v)
			ok = onlyKeys(m, "Type", "Config") && (idle(m["Type"]) || m["Type"] == "json-file") && idle(m["Config"])
		case "RestartPolicy":
			m := object(v)
			name := text(m["Name"])
			ok = onlyKeys(m, "Name", "MaximumRetryCount") && slices.Contains([]string{"", "no", "always", "unless-stopped", "on-failure"}, name) &&
				(idle(m["MaximumRetryCount"]) || name == "on-failure" && integer(m["MaximumRetryCount"]) > 0)
		case "ExtraHosts":
			ok = true // extraHosts validates them
		case "Init":
			// ponytail: there is no docker-init; the runtime already gives PID 1 Linux
			// signal handling and reaps orphans, so --init changes nothing here.
			_, ok = v.(bool)
		}
		if !ok {
			panic(apiError{501, "Unsupported HostConfig: " + k})
		}
	}
	imageName := str(body["Image"])
	reject(len(imageName) > 0 && len(imageName) <= 512, "Invalid image name")
	im := prepared
	if im == nil {
		im = b.image(imageName)
	}
	if wd := text(body["WorkingDir"]); wd != "" {
		reject(strings.HasPrefix(wd, "/"), "WorkingDir must be absolute")
		pathName(wd[1:])
		im.WorkingDir = wd
	}
	if body["Entrypoint"] != nil {
		im.Entry = stringsOf(body["Entrypoint"])
		reject(len(im.Entry) > 0, "Empty Entrypoint unsupported")
	}
	var override []string
	if body["Cmd"] != nil { // [] clears the image's CMD: the Entrypoint runs alone
		override = stringsOf(body["Cmd"])
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
	// The default bridge or a user network: a private address and ports. "host" shares
	// the Mac's network; "none" keeps the container to its own loopback.
	mode := text(host["NetworkMode"])
	for k := range endpoints {
		if k != mode && k != "default" && !(k == "bridge" && (mode == "" || mode == "default")) {
			panic(apiError{501, "Endpoint " + k + " does not match the container's network"})
		}
	}
	if mode != "host" && mode != "none" {
		if mode == "" || mode == "default" {
			mode = "bridge"
		}
		b.network(mode)
		config["net_name"], config["ip"], config["aliases"] = mode, b.assignIP(mode), aliases
	}
	if v := text(body["Hostname"]); v != "" { // docker run -h
		reject(hostnamePattern.MatchString(v), "Invalid hostname")
		config["hostname"] = v
	}
	reject(idle(body["Domainname"]), "Domainname is not supported")
	if body["StopTimeout"] != nil { // docker run --stop-timeout, Compose's stop_grace_period
		t := integer(body["StopTimeout"])
		reject(t >= 0 && t <= 3600, "StopTimeout must be 0 to 3600 seconds")
		config["stop_timeout"] = json.Number(strconv.FormatInt(t, 10))
	}
	if v := text(body["StopSignal"]); v != "" {
		config["stop_signal"] = stopSignal(v)
	}
	if hc := healthcheck(object(body["Healthcheck"]), im.Healthcheck); hc != nil {
		config["healthcheck"] = hc
	}
	if hosts := stringsOf(host["ExtraHosts"]); len(hosts) > 0 { // --add-host name:ip (host-gateway: the Mac)
		reject(len(hosts) <= 64, "Too many extra hosts")
		pairs := []any{}
		for _, h := range hosts {
			name, ip, ok := strings.Cut(h, ":")
			if !ok {
				name, ip, ok = strings.Cut(h, "=")
			}
			reject(ok && hostnamePattern.MatchString(name) && (ip == "host-gateway" || net.ParseIP(ip) != nil), "Invalid extra host: "+h)
			pairs = append(pairs, []any{name, ip})
		}
		config["extra_hosts"] = pairs
	}
	if rp := object(host["RestartPolicy"]); !idle(rp["Name"]) && rp["Name"] != "no" {
		config["restart"] = obj{"name": rp["Name"], "max": rp["MaximumRetryCount"]}
	}
	if body["Tty"] == true { // docker run -t: the container's stdio is a terminal
		config["tty"] = true
		if a, ok := host["ConsoleSize"].([]any); ok && !idle(a[0]) {
			config["console_size"] = a
		}
	}
	if body["OpenStdin"] == true { // docker run -i
		config["open_stdin"], config["stdin_once"] = true, body["StdinOnce"] == true
	}
	if !idle(host["Memory"]) {
		mib := (integer(host["Memory"]) + 1<<20 - 1) >> 20
		reject(mib >= 6 && mib <= 65536, "Memory limit must be 6 MiB to 64 GiB")
		config["memory_mib"] = json.Number(strconv.FormatInt(mib, 10))
	}
	if !idle(host["NanoCpus"]) {
		// ponytail: --cpus sets the vCPU count, rounded up, not a CFS quota.
		cpus := (integer(host["NanoCpus"]) + 1e9 - 1) / 1e9
		reject(cpus <= 64, "At most 64 CPUs")
		config["cpus"] = json.Number(strconv.FormatInt(cpus, 10))
	}
	if host["AutoRemove"] == true {
		config["auto_remove"] = true // docker run --rm: removed once it exits (engine.watch)
	}
	if user := validUser(text(body["User"])); user != "" {
		im.User = user // docker run -u
	}
	if im.User != "" {
		config["user"] = im.User
	}
	if mounts := b.parseMounts(host, body, im.Volumes); len(mounts) > 0 {
		config["mounts"] = mounts
		if host["Binds"] != nil {
			config["binds"] = stringsOf(host["Binds"])
		}
	}
	profile := loadProfileCatalog(profileCatalogJSON).selectForHost(im.Digest, b.Native)
	profile.apply(config)
	validateLaunchProfile(config, b.Native)
	reject(config["mounts"] == nil || config["host_sandbox"] != true, "Mounts are not supported for host-sandboxed containers")
	stage, err := os.MkdirTemp(b.State, ".creating-")
	check(err)
	committed := false
	defer func() {
		if !committed {
			_ = os.RemoveAll(stage)
		}
	}()
	cloneTree(im.Root, filepath.Join(stage, "rootfs"))
	im.Root = filepath.Join(stage, "rootfs")
	for _, d := range append([]string{im.WorkingDir}, im.Volumes...) { // Docker creates them
		im.ensureDir(d)
	}
	save(filepath.Join(stage, "config.json"), config)
	save(filepath.Join(stage, "status.json"), obj{"state": "created"})
	check(os.Rename(stage, p))
	committed = true
	b.refreshHosts(text(config["net_name"]))
	return obj{"Id": key(name), "Warnings": []string{}}
}

var versionPath = regexp.MustCompile(`^/v([0-9]+)\.([0-9]+)(/.*)$`)
var containerPath = regexp.MustCompile(`^/containers/([^/]+)(?:/(json|start|stop|restart|logs|wait|attach|kill|archive|stats|exec|resize|top))?$`)
var execPath = regexp.MustCompile(`^/exec/([0-9a-f]+)/(start|json|resize)$`)

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
		// docker cp into a container: a tar of any size, read as it arrives.
		if m := containerPath.FindStringSubmatch(path); m != nil && m[2] == "archive" && method == "PUT" {
			stream()
			defer endStream()
			b.putArchive(b.resolve(m[1]), q.Get("path"), r.Body, yes("noOverwriteDirNonDir"))
			send(w, 200, nil)
			return
		}
		// docker build (the classic builder): the context tar streams in as it is read.
		if path == "/build" && method == "POST" {
			stream()
			defer endStream()
			b.build(w, r, q)
			return
		}
		// docker load: a docker save tar of any size, read as it arrives.
		if path == "/images/load" && method == "POST" {
			stream()
			defer endStream()
			b.loadImages(w, r.Body, yes("quiet"))
			return
		}
		// docker save: GET /images/get?names=A&names=B, or /images/NAME/get.
		if method == "GET" && (path == "/images/get" || strings.HasPrefix(path, "/images/") && strings.HasSuffix(path, "/get")) {
			names := q["names"]
			if path != "/images/get" {
				names = []string{strings.TrimSuffix(strings.TrimPrefix(path, "/images/"), "/get")}
			}
			stream()
			defer endStream()
			b.saveImages(w, names)
			return
		}
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
		switch {
		case path == "/_ping" && (method == "GET" || method == "HEAD"):
			// The classic builder (POST /build): the docker CLI then builds without BuildKit.
			w.Header().Set("Builder-Version", "1")
			w.Header().Set("Content-Type", "text/plain")
			w.WriteHeader(200)
			_, _ = w.Write([]byte("OK"))
			return
		case path == "/version" && method == "GET":
			send(w, 200, obj{"Version": "0.18.0", "ApiVersion": "1.45", "MinAPIVersion": "1.24", "Os": "linux", "Arch": "arm64", "GitCommit": "preview", "GoVersion": runtime.Version(), "KernelVersion": "dockiva-engine", "Experimental": true, "Components": []any{obj{"Name": "dockivad", "Version": "0.18.0", "Details": obj{"ControlPlane": "Go", "PythonRuntime": "false"}}}})
			return
		case path == "/info" && method == "GET":
			send(w, 200, obj{"ID": "dockiva-engine", "Name": "dockiva-engine", "ServerVersion": "0.18.0", "OSType": "linux", "Architecture": "aarch64", "OperatingSystem": "Dockiva Engine on " + runtime.GOOS, "Driver": "private-copy", "Containers": len(b.names()), "NCPU": min(runtime.NumCPU(), 64), "MemTotal": 0, "SecurityOptions": []any{}, "Warnings": []string{"Not a production isolation boundary"}})
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
		case b.systemAPI(w, method, path, q): // before /containers/NAME: /containers/prune
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
					rows = append(rows, psRow(item))
				}
			}
			send(w, 200, rows)
			return
		case path == "/images/json" && method == "GET":
			defer debug.FreeOSMemory()
			filters := obj{}
			if q.Get("filters") != "" {
				filters = decode([]byte(q.Get("filters")), maxJSON)
			}
			reject(onlyKeys(filters, "reference", "dangling"), "Unsupported image filter")
			entries, e := os.ReadDir(b.Cache)
			check(e)
			rows := []any{}
			for _, entry := range entries {
				if entry.IsDir() && len(entry.Name()) == 64 {
					p := filepath.Join(b.Cache, entry.Name(), "image.json")
					meta := readObject(p)
					if !imageFilter(str(meta["name"]), filters) {
						continue
					}
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
		case strings.HasPrefix(path, "/images/") && strings.HasSuffix(path, "/tag") && method == "POST":
			b.Mu.Lock()
			defer b.Mu.Unlock()
			source := strings.TrimSuffix(strings.TrimPrefix(path, "/images/"), "/tag")
			target := q.Get("repo") + ":" + cmp.Or(q.Get("tag"), "latest")
			reject(q.Get("repo") != "" && len(target) <= 512 && !strings.Contains(target, "@"), "Invalid tag")
			commitImage(b.Cache, target, noLinks(b.imagePath(source), "layout"))
			send(w, 201, nil)
			return
		case strings.HasPrefix(path, "/images/") && method == "DELETE":
			b.Mu.Lock()
			defer b.Mu.Unlock()
			send(w, 200, b.removeImage(strings.TrimPrefix(path, "/images/"), yes("force")))
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
		case strings.HasPrefix(path, "/volumes") && b.volumesAPI(w, method, path, q, raw):
			return
		case strings.HasPrefix(path, "/networks") && b.networksAPI(w, method, path, q, raw):
			return
		case path == "/images/create" && method == "POST":
			defer debug.FreeOSMemory()
			reference := q.Get("fromImage")
			if tag := q.Get("tag"); tag != "" && !strings.Contains(reference, "@") {
				reference += ":" + tag
			}
			// No engine lock (a pull is staged, then renamed in) and no write deadline:
			// a large image takes minutes, and other requests go on meanwhile.
			unbounded(w)
			pullImage(reference, b.Cache, false, "")
			send(w, 200, obj{"status": "Downloaded verified image " + reference})
			return
		}
		if m := execPath.FindStringSubmatch(path); m != nil {
			switch {
			case m[2] == "json" && method == "GET":
				send(w, 200, b.inspectExec(m[1]))
				return
			case m[2] == "resize" && method == "POST":
				h, e := strconv.ParseInt(q.Get("h"), 10, 64)
				check(e)
				wd, e := strconv.ParseInt(q.Get("w"), 10, 64)
				check(e)
				b.resizeExec(m[1], h, wd)
				send(w, 200, nil)
				return
			case m[2] == "start" && method == "POST":
				body := obj{}
				if len(raw) > 0 {
					body = decode(raw, maxJSON)
				}
				stream()
				defer endStream()
				b.startExec(w, r, m[1], body)
				return
			}
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
				unbounded(w) // a long stop grace period
				if action == "stop" || action == "restart" {
					if t := q.Get("t"); t != "" {
						n, e := strconv.Atoi(t)
						reject(e == nil && n >= -1 && n <= 3600, "Invalid stop timeout")
						if n < 0 {
							n = 3600 // -1: wait for the container
						}
						b.stop(name, n)
					} else {
						b.stop(name)
					}
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
				tty := readObject(filepath.Join(p, "config.json"))["tty"] == true
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
					if tty { // a terminal's output is one raw stream
						data = append(data, log...)
					} else {
						data = frame(data, i+1, log)
					}
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
				b.followLogs(w, r, p, yes("stdout"), yes("stderr"), sizes, tty)
				return
			case action == "wait" && method == "POST":
				stream()
				defer endStream()
				b.wait(w, r, name, q.Get("condition"))
				return
			case action == "attach" && method == "POST":
				reject(yes("stream") && !yes("logs"), "Attach supports stream=1 without logs replay")
				stream()
				defer endStream()
				b.attach(w, r, name, yes("stdin"), yes("stdout"), yes("stderr"))
				return
			case action == "resize" && method == "POST":
				h, e := strconv.ParseInt(q.Get("h"), 10, 64)
				check(e)
				wd, e := strconv.ParseInt(q.Get("w"), 10, 64)
				check(e)
				reject(h > 0 && h < 65536 && wd > 0 && wd < 65536, "Invalid terminal size")
				b.resize(name, h, wd)
				send(w, 200, nil)
				return
			case action == "exec" && method == "POST":
				body := obj{}
				if len(raw) > 0 {
					body = decode(raw, maxJSON)
				}
				send(w, 201, b.createExec(name, body))
				return
			case action == "archive" && (method == "GET" || method == "HEAD"):
				stream()
				defer endStream()
				b.getArchive(w, name, q.Get("path"), method == "HEAD")
				return
			case action == "stats" && method == "GET":
				streaming := q.Get("stream") == "" || yes("stream")
				if streaming {
					stream()
					defer endStream()
				}
				b.stats(w, r, name, streaming, yes("one-shot"))
				return
			case action == "top" && method == "GET":
				send(w, 200, b.top(name))
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
				mounts := savedMounts(readObject(filepath.Join(b.container(name), "config.json")))
				b.remove(name)
				if yes("v") { // docker rm -v: the container's anonymous volumes go too
					b.removeAnonymousVolumes(mounts)
				}
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
	if n, ok := c["net_name"].(string); ok {
		return n
	}
	return "host"
}

var hostnamePattern = regexp.MustCompile(`^[a-zA-Z0-9]([a-zA-Z0-9.-]{0,62})$`)
