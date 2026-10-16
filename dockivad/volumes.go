package main

import (
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"syscall"
)

// Bind mounts, volumes and tmpfs mounts. A container saves its mounts in config.json
// as {"type": "bind" | "volume" | "tmpfs", "source": host path or volume name (none
// for tmpfs), "target": guest path, "readonly", "anonymous", "nocopy", "options"
// (tmpfs: its mount options), "flag" (tmpfs: from --tmpfs, shown as HostConfig.Tmpfs)}.
// The supervisor creates their mount points in the container root and passes them to
// the runtime as --bind/--bind-ro, or --tmpfs: a file system the runtime keeps in
// memory (memfs), charged to the container's memory limit as Linux charges tmpfs.
// Volumes live in <home>/volumes/<name>: meta.json and the data in _data.

var volumeName = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,127}$`)

func volumesDir(home string) string { return private(filepath.Join(home, "volumes")) }
func volumeData(home, name string) string {
	reject(volumeName.MatchString(name), "Invalid volume name: "+name)
	return filepath.Join(volumesDir(home), name, "_data")
}

// createVolume makes a volume, or returns the existing one of that name.
func (b *engine) createVolume(name string, labels obj, anonymous bool) obj {
	reject(volumeName.MatchString(name), "Invalid volume name: "+name)
	reject(len(labels) <= 64, "Too many labels")
	dir := filepath.Join(volumesDir(b.Home), name)
	if !exists(dir) {
		stage, e := os.MkdirTemp(volumesDir(b.Home), ".creating-")
		check(e)
		check(mkdirGuest(filepath.Join(stage, "_data")))
		save(filepath.Join(stage, "meta.json"), obj{"name": name, "created_at": now(), "labels": labels, "anonymous": anonymous, "populated": false})
		if os.Rename(stage, dir) != nil { // created meanwhile
			_ = os.RemoveAll(stage)
		}
	}
	return b.volumeInfo(name)
}
func (b *engine) volumeMeta(name string) obj {
	reject(volumeName.MatchString(name), "Invalid volume name: "+name)
	p := filepath.Join(volumesDir(b.Home), name, "meta.json")
	if !exists(p) {
		panic(apiError{404, "No such volume: " + name})
	}
	return readObject(p)
}
func (b *engine) volumeInfo(name string) obj {
	meta := b.volumeMeta(name)
	labels := object(meta["labels"])
	return obj{"Name": name, "Driver": "local", "Mountpoint": volumeData(b.Home, name), "CreatedAt": timestamp(meta["created_at"]),
		"Labels": labels, "Scope": "local", "Options": obj{}}
}
func (b *engine) volumeNames() []string {
	entries, e := os.ReadDir(volumesDir(b.Home))
	check(e)
	out := []string{}
	for _, entry := range entries {
		if entry.IsDir() && volumeName.MatchString(entry.Name()) && exists(filepath.Join(volumesDir(b.Home), entry.Name(), "meta.json")) {
			out = append(out, entry.Name())
		}
	}
	return out
}

// volumeUsers lists the containers that mount a volume.
func (b *engine) volumeUsers(name string) []string {
	users := []string{}
	for _, c := range b.names() {
		var config obj
		if safeRun(func() { config = readObject(filepath.Join(b.container(c), "config.json")) }) != nil {
			continue
		}
		for _, m := range savedMounts(config) {
			if m["type"] == "volume" && m["source"] == name {
				users = append(users, c)
				break
			}
		}
	}
	return users
}
func (b *engine) removeVolume(name string) {
	b.volumeMeta(name)
	if users := b.volumeUsers(name); len(users) > 0 {
		panic(apiError{409, "Volume " + name + " is in use by " + strings.Join(users, ", ")})
	}
	check(os.RemoveAll(filepath.Join(volumesDir(b.Home), name)))
}

// removeAnonymousVolumes deletes the anonymous volumes a removed container used
// (docker rm -v, and --rm).
func (b *engine) removeAnonymousVolumes(mounts []obj) {
	for _, m := range mounts {
		if m["type"] == "volume" && m["anonymous"] == true {
			_ = safeRun(func() { b.removeVolume(str(m["source"])) })
		}
	}
}
func savedMounts(config obj) []obj {
	out := []obj{}
	if config["mounts"] == nil {
		return out
	}
	for _, v := range array(config["mounts"]) {
		out = append(out, object(v))
	}
	return out
}

// mountTarget validates a guest mount path: absolute, clean, printable UTF-8
// without ':' or '\', and not the root itself.
func mountTarget(t string) string {
	reject(strings.HasPrefix(t, "/") && t != "/" && filepath.Clean(t) == t && len(t) <= 1024, "Invalid mount target: "+t)
	pathName(t[1:])
	return t
}

// parseMounts turns HostConfig.Binds, HostConfig.Mounts and Config.Volumes into
// saved mounts. -v creates a missing host directory (as Docker does); --mount
// requires the source to exist. Volumes are created on first use.
func (b *engine) parseMounts(host, body obj, declared []string) []any {
	mounts := []any{}
	targets := map[string]bool{}
	add := func(m obj) {
		t := mountTarget(str(m["target"]))
		reject(!targets[t], "Duplicate mount point: "+t)
		targets[t] = true
		if m["type"] == "volume" {
			b.createVolume(str(m["source"]), obj{}, m["anonymous"] == true)
		}
		mounts = append(mounts, m)
	}
	bindSource := func(source string, create bool) string {
		reject(filepath.IsAbs(source) && len(source) <= 1024, "Bind source must be an absolute host path: "+source)
		source = filepath.Clean(source)
		st, e := os.Stat(source)
		if os.IsNotExist(e) && create {
			check(os.MkdirAll(source, 0o755))
			st, e = os.Stat(source)
		}
		if e != nil {
			panic(apiError{400, "Bind source path does not exist: " + source})
		}
		reject(st.IsDir() || st.Mode().IsRegular(), "Bind source must be a directory or a regular file: "+source)
		return source
	}
	if host["Binds"] != nil {
		for _, spec := range stringsOf(host["Binds"]) {
			parts := strings.Split(spec, ":")
			reject(len(parts) == 2 || len(parts) == 3, "Invalid bind (expected source:target[:options]): "+spec)
			m := obj{"target": parts[1], "readonly": false}
			if strings.HasPrefix(parts[0], "/") {
				m["type"], m["source"] = "bind", bindSource(parts[0], true)
			} else {
				m["type"], m["source"] = "volume", parts[0]
			}
			if len(parts) == 3 {
				for _, option := range strings.Split(parts[2], ",") {
					switch option {
					case "ro":
						m["readonly"] = true
					case "rw", "z", "Z", "cached", "delegated", "consistent": // no effect on a Mac
					case "nocopy":
						reject(m["type"] == "volume", "nocopy applies to volumes only")
						m["nocopy"] = true
					default:
						panic(apiError{501, "Unsupported bind option: " + option})
					}
				}
			}
			add(m)
		}
	}
	if host["Tmpfs"] != nil { // docker run --tmpfs TARGET[:OPTIONS]
		tmpfs := object(host["Tmpfs"])
		targets := []string{}
		for t := range tmpfs {
			targets = append(targets, t)
		}
		sort.Strings(targets)
		for _, t := range targets {
			value, isText := tmpfs[t].(string)
			reject(isText, "Invalid tmpfs options for "+t)
			add(obj{"type": "tmpfs", "target": t, "options": tmpfsOptions(value), "given": value, "readonly": false, "flag": true})
		}
	}
	if host["Mounts"] != nil {
		for _, raw := range array(host["Mounts"]) {
			spec := object(raw)
			reject(onlyKeys(spec, "Type", "Source", "Target", "ReadOnly", "Consistency", "BindOptions", "VolumeOptions", "TmpfsOptions"), "Unsupported mount option")
			m := obj{"target": str(spec["Target"]), "readonly": spec["ReadOnly"] == true}
			switch spec["Type"] {
			case "bind":
				options := object(spec["BindOptions"])
				reject(onlyKeys(options, "Propagation", "CreateMountpoint") && (idle(options["Propagation"]) || options["Propagation"] == "rprivate"), "Unsupported bind mount option")
				m["type"], m["source"] = "bind", bindSource(str(spec["Source"]), options["CreateMountpoint"] == true)
			case "volume":
				options := object(spec["VolumeOptions"])
				reject(onlyKeys(options, "NoCopy", "Labels", "DriverConfig") && idle(options["DriverConfig"]), "Unsupported volume option")
				name := text(spec["Source"])
				if name == "" {
					name, m["anonymous"] = token(), true
				}
				m["type"], m["source"] = "volume", name
				if options["NoCopy"] == true {
					m["nocopy"] = true
				}
			case "tmpfs": // --mount type=tmpfs[,tmpfs-size=,tmpfs-mode=]
				reject(idle(spec["Source"]), "A tmpfs mount has no source")
				options := object(spec["TmpfsOptions"])
				reject(onlyKeys(options, "SizeBytes", "Mode", "Options"), "Unsupported tmpfs option")
				list := []string{}
				if !idle(options["SizeBytes"]) {
					reject(integer(options["SizeBytes"]) > 0, "Invalid tmpfs size")
					list = append(list, "size="+strconv.FormatInt(integer(options["SizeBytes"]), 10))
				}
				if !idle(options["Mode"]) {
					mode := integer(options["Mode"])
					reject(mode >= 0 && mode <= 0o7777, "Invalid tmpfs mode")
					list = append(list, "mode="+strconv.FormatInt(mode, 8))
				}
				for _, raw := range optionalArray(options["Options"]) { // [["exec"], ["nr_inodes", "1000"]]
					parts := stringsOf(raw)
					reject(len(parts) == 1 || len(parts) == 2, "Invalid tmpfs option")
					list = append(list, strings.Join(parts, "="))
				}
				if m["readonly"] == true {
					list = append(list, "ro")
				}
				m["type"], m["options"] = "tmpfs", tmpfsOptions(strings.Join(list, ","))
			default:
				panic(apiError{501, "Unsupported mount type: " + text(spec["Type"]) + " (bind, volume and tmpfs are supported)"})
			}
			add(m)
		}
	}
	// docker run -v /path (Config.Volumes) and the image's VOLUMEs: anonymous volumes.
	anonymous := append([]string{}, declared...)
	for target := range object(body["Volumes"]) {
		anonymous = append(anonymous, target)
	}
	sort.Strings(anonymous)
	for _, target := range anonymous {
		if target = filepath.Clean(target); !targets[target] {
			add(obj{"type": "volume", "source": token(), "target": target, "readonly": false, "anonymous": true})
		}
	}
	return mounts
}

// tmpfsOptions validates tmpfs mount options (Docker's: size, mode, uid, gid,
// nr_inodes, exec, noexec and flags without effect here) and puts Docker's defaults,
// noexec, nosuid and nodev, first (later options win).
func tmpfsOptions(value string) string {
	out := []string{"noexec", "nosuid", "nodev"}
	for _, option := range strings.Split(value, ",") {
		if option == "" {
			continue
		}
		key, v, hasValue := strings.Cut(option, "=")
		switch key {
		case "size":
			reject(hasValue && tmpfsSize.MatchString(v), "Invalid tmpfs size: "+v)
		case "mode":
			n, e := strconv.ParseUint(v, 8, 32)
			reject(hasValue && e == nil && n <= 0o7777, "Invalid tmpfs mode: "+v)
		case "uid", "gid", "nr_inodes":
			_, e := strconv.ParseUint(v, 10, 32)
			reject(hasValue && e == nil, "Invalid tmpfs "+key+": "+v)
		case "exec", "noexec", "suid", "nosuid", "dev", "nodev", "rw", "ro", "atime", "noatime", "relatime", "norelatime", "strictatime", "diratime", "nodiratime", "sync", "async", "mand", "nomand":
			reject(!hasValue, "Invalid tmpfs option: "+option)
		default:
			panic(apiError{501, "Unsupported tmpfs option: " + option})
		}
		out = append(out, option)
	}
	return strings.Join(out, ",")
}

var tmpfsSize = regexp.MustCompile(`^[0-9]{1,15}[kKmMgG%]?$`)

// inspectTmpfs: HostConfig.Tmpfs, the --tmpfs mounts and their options as given.
func inspectTmpfs(config obj) obj {
	out := obj{}
	for _, m := range savedMounts(config) {
		if m["type"] == "tmpfs" && m["flag"] == true {
			out[str(m["target"])] = text(m["given"])
		}
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

// inspectMounts renders saved mounts as Docker's Mounts list (--tmpfs mounts are in HostConfig.Tmpfs).
func (b *engine) inspectMounts(config obj) []any {
	out := []any{}
	for _, m := range savedMounts(config) {
		if m["type"] == "tmpfs" {
			if m["flag"] != true {
				out = append(out, obj{"Type": "tmpfs", "Source": "", "Destination": m["target"], "Mode": "", "RW": m["readonly"] != true, "Propagation": ""})
			}
			continue
		}
		item := obj{"Type": m["type"], "Destination": m["target"], "Mode": "", "RW": m["readonly"] != true, "Propagation": "rprivate"}
		if m["type"] == "volume" {
			item["Name"], item["Driver"], item["Source"] = m["source"], "local", filepath.Join(b.Home, "volumes", str(m["source"]), "_data")
		} else {
			item["Source"] = m["source"]
		}
		if m["readonly"] == true {
			item["Mode"] = "ro"
		}
		out = append(out, item)
	}
	return out
}

// prepareMounts, in the supervisor: the runtime arguments for a container's
// mounts. Mount points are created in the container root (resolving its
// symlinks, as Docker does), and a new volume first receives what the image has
// at its mount point unless nocopy is set. tmpfs: whether this run gets the tmpfs
// mounts; only the container's own run does (an exec or healthcheck is a host
// process of its own and cannot share the run's memory: it sees the mount point).
func prepareMounts(home, root string, config obj, tmpfs bool) []string {
	args := []string{}
	link := func(p string) (string, bool) {
		s, e := os.Lstat(filepath.Join(root, p))
		if e != nil || s.Mode()&os.ModeSymlink == 0 {
			return "", false
		}
		target, e := os.Readlink(filepath.Join(root, p))
		check(e)
		return target, true
	}
	for _, m := range savedMounts(config) {
		if m["type"] == "tmpfs" {
			guest := resolveGuest(pathName(mountTarget(str(m["target"]))[1:]), link)
			reject(guest != "", "A mount cannot replace the container root")
			mountPoint(root, guest, true)
			if tmpfs {
				args = append(args, "--tmpfs", "/"+guest+":"+str(m["options"]))
			}
			continue
		}
		source := str(m["source"])
		if m["type"] == "volume" {
			source = volumeData(home, source)
		}
		st, e := os.Stat(source)
		if e != nil {
			panic(apiError{409, "Mount source is missing: " + source})
		}
		guest := resolveGuest(pathName(mountTarget(str(m["target"]))[1:]), link)
		reject(guest != "", "A mount cannot replace the container root")
		mountPoint(root, guest, st.IsDir())
		if m["type"] == "volume" && m["nocopy"] != true {
			populateVolume(home, str(m["source"]), filepath.Join(root, guest))
		}
		flag := "--bind"
		if m["readonly"] == true {
			flag = "--bind-ro"
		}
		args = append(args, flag, source, "/"+guest)
	}
	return args
}

func optionalArray(v any) []any {
	if idle(v) {
		return nil
	}
	return array(v)
}

// hasMount: whether config mounts something at target.
func hasMount(config obj, target string) bool {
	for _, m := range savedMounts(config) {
		if m["target"] == target {
			return true
		}
	}
	return false
}

// mountPoint creates a link-free directory (or empty file) at guest within root.
func mountPoint(root, guest string, directory bool) {
	parts := strings.Split(guest, "/")
	p := root
	for i, part := range parts {
		p = filepath.Join(p, part)
		last := i == len(parts)-1
		st, e := os.Lstat(p)
		if os.IsNotExist(e) {
			if last && !directory {
				f, e := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, 0o644)
				check(e)
				f.Close()
			} else {
				check(mkdirGuest(p))
			}
			continue
		}
		check(e)
		reject(st.Mode()&os.ModeSymlink == 0, "Mount point is a symlink: /"+guest)
		reject(!last && st.IsDir() || last && st.IsDir() == directory, "Mount point type does not match its source: /"+guest)
	}
}

// populateVolume copies the image's content at a mount point into a volume that
// has never been used, as Docker does for a new named or anonymous volume.
func populateVolume(home, name, from string) {
	meta := filepath.Join(volumesDir(home), name, "meta.json")
	m := readObject(meta)
	if m["populated"] == true {
		return
	}
	data := volumeData(home, name)
	entries, e := os.ReadDir(data)
	check(e)
	if st, e := os.Lstat(from); len(entries) == 0 && e == nil && st.IsDir() {
		copyTree(from, data)
	}
	m["populated"] = true
	save(meta, m)
}

// copyTree copies files, directories and symlinks (as symlinks) from one tree to another.
func copyTree(from, to string) {
	check(filepath.Walk(from, func(p string, info os.FileInfo, err error) error {
		check(err)
		rel, e := filepath.Rel(from, p)
		check(e)
		if rel == "." {
			return nil
		}
		dest := filepath.Join(to, rel)
		switch {
		case info.Mode()&os.ModeSymlink != 0:
			target, e := os.Readlink(p)
			check(e)
			check(os.Symlink(target, dest))
		case info.IsDir():
			check(os.Mkdir(dest, 0o700))
			check(setMode(dest, info.Mode()&permBits|0o700))
		case info.Mode().IsRegular():
			in, e := os.Open(p)
			check(e)
			defer in.Close()
			out, e := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, info.Mode().Perm())
			check(e)
			defer out.Close()
			_, e = io.Copy(out, in)
			check(e)
		}
		return nil
	}))
}

// volumesAPI serves /volumes, /volumes/create, /volumes/prune and /volumes/{name}.
func (b *engine) volumesAPI(w http.ResponseWriter, method, path string, q map[string][]string, raw []byte) bool {
	query := func(k string) string {
		if v := q[k]; len(v) > 0 {
			return v[0]
		}
		return ""
	}
	switch {
	case path == "/volumes" && method == "GET":
		filters := obj{}
		if f := query("filters"); f != "" {
			filters = decode([]byte(f), maxJSON)
		}
		reject(onlyKeys(filters, "name", "dangling", "label"), "Unsupported volume filter")
		rows := []any{}
		for _, name := range b.volumeNames() {
			if names := filterValues(filters["name"]); len(names) > 0 && !anyContains(names, name) {
				continue
			}
			if dangling := filterValues(filters["dangling"]); len(dangling) > 0 {
				unused := len(b.volumeUsers(name)) == 0
				if (dangling[0] == "true" || dangling[0] == "1") != unused {
					continue
				}
			}
			info := b.volumeInfo(name)
			if !labelsMatch(filterValues(filters["label"]), object(info["Labels"])) {
				continue
			}
			rows = append(rows, info)
		}
		send(w, 200, obj{"Volumes": rows, "Warnings": []any{}})
	case path == "/volumes/create" && method == "POST":
		body := obj{}
		if len(raw) > 0 {
			body = decode(raw, maxJSON)
		}
		reject(onlyKeys(body, "Name", "Driver", "DriverOpts", "Labels", "ClusterVolumeSpec"), "Unsupported volume option")
		reject(idle(body["Driver"]) || body["Driver"] == "local", "Only the local volume driver is supported")
		reject(idle(body["DriverOpts"]) && idle(body["ClusterVolumeSpec"]), "Volume driver options are not supported")
		b.Mu.Lock()
		defer b.Mu.Unlock()
		name, anonymous := text(body["Name"]), false
		if name == "" {
			name, anonymous = token(), true
		}
		send(w, 201, b.createVolume(name, object(body["Labels"]), anonymous))
	case path == "/volumes/prune" && method == "POST":
		filters := obj{}
		if f := query("filters"); f != "" {
			filters = decode([]byte(f), maxJSON)
		}
		reject(onlyKeys(filters, "all"), "Unsupported prune filter")
		all := false
		for _, v := range filterValues(filters["all"]) {
			all = all || v == "true" || v == "1"
		}
		b.Mu.Lock()
		defer b.Mu.Unlock()
		deleted := []string{}
		for _, name := range b.volumeNames() {
			// Like Docker 23+: only anonymous volumes, unless all=true.
			if (all || b.volumeMeta(name)["anonymous"] == true) && len(b.volumeUsers(name)) == 0 {
				if safeRun(func() { b.removeVolume(name) }) == nil {
					deleted = append(deleted, name)
				}
			}
		}
		send(w, 200, obj{"VolumesDeleted": deleted, "SpaceReclaimed": 0})
	case strings.HasPrefix(path, "/volumes/") && (method == "GET" || method == "DELETE"):
		name := strings.TrimPrefix(path, "/volumes/")
		if method == "GET" {
			send(w, 200, b.volumeInfo(name))
			return true
		}
		b.Mu.Lock()
		defer b.Mu.Unlock()
		if !exists(filepath.Join(volumesDir(b.Home), name, "meta.json")) && (query("force") == "1" || query("force") == "true") {
			send(w, 204, nil)
			return true
		}
		b.removeVolume(name)
		send(w, 204, nil)
	default:
		return false
	}
	return true
}
func filterValues(v any) []string {
	out := []string{}
	switch x := v.(type) {
	case map[string]any:
		for k := range x {
			out = append(out, k)
		}
	case []any:
		for _, s := range x {
			out = append(out, str(s))
		}
	}
	return out
}
func anyContains(values []string, s string) bool {
	for _, v := range values {
		if strings.Contains(s, v) {
			return true
		}
	}
	return false
}
