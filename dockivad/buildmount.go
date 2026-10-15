package main

// RUN's BuildKit options (Dockerfile 1.2+ syntax) on the classic builder:
//
//	--mount=type=cache   a directory kept across builds, keyed by id (the target by
//	                     default): a build cache entry (<home>/build-cache/<key>/data,
//	                     Type exec.cachemount), so docker builder prune, buildx du
//	                     and system df count it. It is not part of the step's key.
//	--mount=type=bind    a path of the context, a stage or an image (from=):
//	                     read-only, or with rw a clone whose changes are discarded
//	--mount=type=tmpfs   an empty directory, discarded after the step
//	--mount=type=secret  a secret from the client's session (docker build --secret),
//	                     as a read-only file and/or an environment variable (env=)
//	--network            default, none or host; --security=sandbox (insecure is refused)
//
// Each mount is a bind mount into the step's container. The mount points the
// runtime makes in the stage's files are removed after the step, as BuildKit does.
// ponytail: sharing=private is shared (BuildKit makes a second cache while one is
// in use); uid and gid are ignored (guest files belong to the container user);
// type=ssh is refused (no agent forwarding).

import (
	"encoding/csv"
	"os"
	"path"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type runMount struct {
	kind     string // bind, cache, tmpfs, secret, or script (a RUN here-document starting #!)
	target   string // absolute guest path; "" for a secret given as env= only
	readonly bool
	from     string // bind and cache: a stage or image the source is in (the context by default)
	source   string // bind and cache: the path in from
	host     string // bind, and cache with from: the source's host path
	id       string // cache: the cache's name; secret: the secret's
	sharing  string // cache: shared, private or locked
	mode     int64  // cache and secret: permissions, -1 when not given
	required bool   // secret: fail when the client has no such secret
	env      string // secret: an environment variable set to the value
	script   string // script: the file
}

// runOptions is what a RUN's flags and here-documents ask for.
type runOptions struct {
	command   []any
	mounts    []runMount // by target, parents first
	network   string
	extra     string // the step's cache key's share: the bind mounts' sources
	cacheable bool
}

func (bd *builder) runOptions(st *buildStage, in instruction, env map[string]string) runOptions {
	o := runOptions{network: bd.network, cacheable: true}
	for name, v := range in.flags {
		v = processWords(v, env, bd.escape, false)[0]
		switch name {
		case "network":
			reject(v == "default" || v == "none" || v == "host", "RUN --network takes default, none or host, not "+v)
			if v != "default" {
				o.network = v
			}
		case "security":
			reject(v != "insecure", "RUN --security=insecure needs a privileged build container, which Dockiva Engine's builder cannot run")
			reject(v == "sandbox", "RUN --security takes sandbox or insecure, not "+v)
		default:
			panic(apiError{400, "RUN --" + name + " is not supported"})
		}
	}
	var script *runMount
	o.command, script = bd.runCommand(st, in)
	if script != nil {
		o.mounts = append(o.mounts, *script)
	}
	for _, spec := range in.mounts {
		m := parseMount(spec, env, bd.escape, stageWorkdir(st))
		part, ok := bd.resolveMount(st, &m)
		o.extra += part + "\n"
		o.cacheable = o.cacheable && ok
		o.mounts = append(o.mounts, m)
	}
	sort.SliceStable(o.mounts, func(i, j int) bool { return o.mounts[i].target < o.mounts[j].target })
	for i := 1; i < len(o.mounts); i++ {
		reject(o.mounts[i].target == "" || o.mounts[i].target != o.mounts[i-1].target, "RUN mounts two things at "+o.mounts[i].target)
	}
	return o
}

// runCommand is RUN's command. A here-document that is the whole command is the
// script, run by its #! line or else by the shell; otherwise the shell gets the
// command line followed by the documents and does the redirecting (BuildKit's rules).
func (bd *builder) runCommand(st *buildStage, in instruction) ([]any, *runMount) {
	if len(in.heredocs) == 0 {
		return bd.command(st, in), nil
	}
	shell := stageShell(st)
	if len(in.heredocs) == 1 && len(rawWords(in.rest)) == 1 {
		h := in.heredocs[0]
		script := h.content()
		if strings.HasPrefix(script, "#!") {
			// The engine finds a command's interpreter in the stage's files, where the
			// script (a mount) is not: run the #! line's interpreter on it, as execve does.
			target := "/dev/pipes/" + h.name
			line, _, _ := strings.Cut(script[2:], "\n")
			interpreter, arg, _ := strings.Cut(strings.TrimSpace(line), " ")
			reject(interpreter != "", "RUN here-document: #! names no interpreter")
			command := []string{interpreter}
			if arg = strings.TrimSpace(arg); arg != "" {
				command = append(command, arg)
			}
			return anyStrings(append(command, target)), &runMount{kind: "script", target: mountTarget(target), readonly: true, mode: -1, script: script}
		}
		return anyStrings(append(shell, script)), nil
	}
	full := in.rest
	for _, h := range in.heredocs {
		full += "\n" + h.body + h.name
	}
	return anyStrings(append(shell, full)), nil
}

// parseMount reads one --mount value as BuildKit does: CSV fields, key=value or a
// bare flag (ro, rw, required); values expand variables.
func parseMount(spec string, env map[string]string, escape byte, workdir string) runMount {
	fields, e := csv.NewReader(strings.NewReader(spec)).Read()
	reject(e == nil && len(fields) > 0, "Invalid RUN --mount="+spec)
	m := runMount{kind: "bind", mode: -1}
	given := map[string]bool{}
	boolean := func(k, v string) bool {
		b, e := strconv.ParseBool(v)
		reject(e == nil, "RUN --mount "+k+"= takes true or false, not "+v)
		return b
	}
	for _, f := range fields {
		k, v, hasValue := strings.Cut(f, "=")
		k = strings.ToLower(strings.TrimSpace(k))
		if !hasValue {
			reject(k == "ro" || k == "readonly" || k == "rw" || k == "readwrite" || k == "required", "RUN --mount: "+f+" needs a value, in "+spec)
			v = "true"
		}
		v = processWords(v, env, escape, false)[0]
		given[k] = true
		switch k {
		case "type":
			m.kind = v
		case "from":
			m.from = v
		case "source", "src":
			m.source, given["source"] = v, true
		case "target", "dst", "destination":
			m.target = v
		case "ro", "readonly":
			m.readonly, given["ro"] = boolean(k, v), true
		case "rw", "readwrite":
			m.readonly, given["ro"] = !boolean(k, v), true
		case "required":
			m.required = boolean(k, v)
		case "id":
			m.id = v
		case "sharing":
			m.sharing = v
			reject(v == "shared" || v == "private" || v == "locked", "RUN --mount sharing= takes shared, private or locked, not "+v)
		case "mode":
			n, e := strconv.ParseUint(v, 8, 32)
			reject(e == nil && n <= 0o777, "RUN --mount mode= takes octal permissions, not "+v)
			m.mode = int64(n)
		case "uid", "gid":
			_, e := strconv.ParseUint(v, 10, 32)
			reject(e == nil, "RUN --mount "+k+"= takes a number, not "+v)
		case "env":
			m.env = v
		case "size": // tmpfs: no size limit here
		default:
			panic(apiError{400, "RUN --mount: unknown option " + k + " in " + spec})
		}
	}
	switch m.kind {
	case "bind", "cache", "tmpfs", "secret":
	case "ssh":
		panic(apiError{400, "RUN --mount=type=ssh needs SSH agent forwarding, which Dockiva Engine's builder does not support"})
	default:
		panic(apiError{400, "Unsupported RUN --mount type: " + m.kind + " (bind, cache, tmpfs and secret are supported)"})
	}
	only := func(key string, kinds ...string) {
		for _, k := range kinds {
			if m.kind == k {
				return
			}
		}
		reject(!given[key], "RUN --mount "+key+"= does not apply to type="+m.kind)
	}
	only("from", "bind", "cache")
	only("source", "bind", "cache", "secret")
	only("id", "cache", "secret")
	only("sharing", "cache")
	only("mode", "cache", "secret")
	only("uid", "cache", "secret")
	only("gid", "cache", "secret")
	only("required", "secret")
	only("env", "secret")
	only("size", "tmpfs")
	if !given["ro"] {
		m.readonly = m.kind == "bind" || m.kind == "secret"
	}
	if m.kind == "secret" {
		reject(m.source == "" || m.id == "", "A secret mount takes id= or source=, not both")
		if m.source != "" {
			m.id, m.source = m.source, ""
		}
		if m.id == "" {
			reject(m.target != "", "A secret mount needs an id or a target")
			m.id = path.Base(m.target)
		}
		if m.target == "" && m.env == "" {
			m.target = "/run/secrets/" + path.Base(m.id)
		}
		m.readonly = true
	}
	if m.target != "" {
		if !strings.HasPrefix(m.target, "/") {
			m.target = path.Join(workdir, m.target)
		}
		m.target = mountTarget(path.Clean(m.target))
	} else {
		reject(m.kind == "secret", "RUN --mount=type="+m.kind+" needs a target")
	}
	if m.kind == "cache" && m.id == "" {
		m.id = m.target
	}
	return m
}

// resolveMount finds a bind (or seeded cache) mount's source and returns what the
// step's cache key adds for it: the context's files, or the stage's state or
// image's digest. ok is false for a stage that is not cached.
func (bd *builder) resolveMount(st *buildStage, m *runMount) (part string, ok bool) {
	if m.kind != "bind" && !(m.kind == "cache" && m.from != "") {
		return "", true
	}
	root, where, base := bd.context, "the build context", ""
	if m.from != "" {
		if s := bd.findStage(m.from); s != nil {
			reject(s != st, "RUN --mount from= cannot name the current stage")
			root, where, base = s.root, "stage "+m.from, "stage "+s.cacheID
			ok = s.cacheID != ""
		} else {
			im := bd.pull(m.from)
			root, where, base, ok = im.Root, m.from, "image "+im.Digest, true
		}
	}
	m.host = filepath.Join(root, guestRel(root, m.source))
	info, e := os.Stat(m.host)
	reject(e == nil, "RUN --mount: "+m.source+" not found in "+where)
	if m.kind == "cache" { // a cache's seed is not part of the key
		reject(info.IsDir(), "RUN --mount=type=cache: "+m.source+" in "+where+" is not a directory")
		return "", true
	}
	if m.from != "" {
		return base + " " + path.Clean("/"+m.source), ok
	}
	return "context " + treeHash(m.host), true
}

// mountSources makes each mount's host side and returns the step container's
// HostConfig.Mounts, its extra environment (secrets given as env=), the targets
// mounted, and release, which ends the mounts' use.
func (bd *builder) mountSources(mounts []runMount) (binds []any, env []string, targets []string, release func()) {
	var undo []func()
	release = func() {
		for i := len(undo) - 1; i >= 0; i-- {
			undo[i]()
		}
	}
	done := false
	defer func() {
		if !done {
			release()
		}
	}()
	scratch := func() string {
		d, e := os.MkdirTemp(bd.work, "mount-")
		check(e)
		undo = append(undo, func() { os.RemoveAll(d) }) // at once: secrets are among them
		return d
	}
	bind := func(source, target string, readonly bool) {
		binds = append(binds, obj{"Type": "bind", "Source": source, "Target": target, "ReadOnly": readonly})
		targets = append(targets, target)
	}
	// sharing=locked: one step at a time per cache, locked in key order so two
	// builds never wait on each other.
	locked := []string{}
	for _, m := range mounts {
		if m.kind == "cache" && m.sharing == "locked" && !slices.Contains(locked, cacheMountKey(m.id)) {
			locked = append(locked, cacheMountKey(m.id))
		}
	}
	sort.Strings(locked)
	for _, k := range locked {
		mu, _ := cacheMountLocks.LoadOrStore(k, &sync.Mutex{})
		mu.(*sync.Mutex).Lock()
		undo = append(undo, mu.(*sync.Mutex).Unlock)
	}
	for _, m := range mounts {
		switch m.kind {
		case "bind":
			source := m.host
			if !m.readonly { // rw: a clone, discarded
				source = filepath.Join(scratch(), "source")
				cloneTree(m.host, source)
			}
			bind(source, m.target, m.readonly)
		case "tmpfs": // the engine's umask is 077: modes are set, not created
			d := scratch()
			check(setMode(d, 0o777|os.ModeSticky))
			bind(d, m.target, m.readonly)
		case "script":
			f := filepath.Join(scratch(), "script")
			check(os.WriteFile(f, []byte(m.script), 0o700))
			check(os.Chmod(f, 0o755))
			bind(f, m.target, true)
		case "cache":
			key, data := bd.b.useCacheMount(bd.work, m)
			undo = append(undo, func() { bd.b.releaseCacheMount(key) })
			bind(data, m.target, m.readonly)
		case "secret":
			value, why := bd.secretValue(m.id)
			if why != "" {
				reject(!m.required, "RUN --mount=type=secret,id="+m.id+": "+why)
				continue // optional: no file, no variable
			}
			if m.env != "" {
				env = append(env, m.env+"="+string(value))
			}
			if m.target != "" {
				mode := os.FileMode(0o400)
				if m.mode >= 0 {
					mode = os.FileMode(m.mode)
				}
				f := filepath.Join(scratch(), "secret")
				check(os.WriteFile(f, value, 0o600))
				check(os.Chmod(f, mode|0o400))
				bind(f, m.target, true)
			}
		}
	}
	done = true
	return binds, env, targets, release
}

// secretValue is a secret from the client's session, or why there is none.
func (bd *builder) secretValue(id string) ([]byte, string) {
	if bd.secret == nil {
		return nil, "no such secret: the classic builder (DOCKER_BUILDKIT=0) has none; build with buildx and docker build --secret id=" + id + ",src=FILE"
	}
	value, e := bd.secret(id)
	if e != nil {
		why := "no such secret; pass it with docker build --secret id=" + id + ",src=FILE"
		if !strings.Contains(e.Error(), "code = NotFound") {
			why += " (" + e.Error() + ")"
		}
		return nil, why
	}
	return value, ""
}

// mountStubs are the paths under root that a container's mount points at targets
// would create (they do not exist yet), deepest first.
func mountStubs(root string, targets []string) []string {
	stubs := []string{}
	for _, t := range targets {
		p := ""
		for _, part := range strings.Split(guestRel(root, t), "/") {
			p = path.Join(p, part)
			if !exists(filepath.Join(root, p)) && !slices.Contains(stubs, p) {
				stubs = append(stubs, p)
			}
		}
	}
	sort.Slice(stubs, func(i, j int) bool { return len(stubs[i]) > len(stubs[j]) })
	return stubs
}

// removeStubs removes the mount points left in a step's files: empty directories
// and empty files only, so what the step wrote there stays.
func removeStubs(root string, stubs []string) {
	for _, s := range stubs {
		_ = safeRun(func() {
			p := filepath.Join(noLinks(root, path.Dir(s)), path.Base(s))
			if st, e := os.Lstat(p); e == nil && (st.IsDir() || st.Mode().IsRegular() && st.Size() == 0) {
				_ = os.Remove(p)
			}
		})
	}
}

// buildCacheInUse counts the running steps that mount each cache; prunes and
// evictions leave those entries. Guarded by buildCacheMu.
var buildCacheInUse = map[string]int{}

// cacheMountLocks are the sharing=locked caches' locks, by key.
var cacheMountLocks sync.Map

func cacheMountKey(id string) string { return key("cache-mount\x00" + id) }

// useCacheMount returns a cache mount's entry key and directory, made on first
// use (empty, or a clone of from's source).
func (b *engine) useCacheMount(work string, m runMount) (string, string) {
	k := cacheMountKey(m.id)
	dir := filepath.Join(b.buildCacheDir(), k)
	buildCacheMu.Lock()
	defer buildCacheMu.Unlock()
	if !exists(filepath.Join(dir, "entry.json")) {
		tmp, e := os.MkdirTemp(work, "cache-")
		check(e)
		data := filepath.Join(tmp, "data")
		if m.host != "" {
			cloneTree(m.host, data)
		} else {
			check(os.Mkdir(data, 0o700))
		}
		if m.mode >= 0 {
			check(setMode(data, os.FileMode(m.mode)|0o700))
		} else if m.host == "" {
			check(setMode(data, 0o755)) // past the engine's umask
		}
		now := time.Now().UTC().Format(time.RFC3339Nano)
		save(filepath.Join(tmp, "entry.json"), obj{"ID": k, "Type": "exec.cachemount", "Description": "cached mount " + m.target + " (id " + m.id + ")",
			"InUse": false, "Shared": m.sharing != "private", "Size": 0, "CreatedAt": now, "LastUsedAt": now, "UsageCount": 0})
		b.discard(dir)
		check(os.Rename(tmp, dir))
	}
	buildCacheInUse[k]++
	return k, filepath.Join(dir, "data")
}

// releaseCacheMount ends a step's use of a cache mount and records its size.
func (b *engine) releaseCacheMount(k string) {
	dir := filepath.Join(b.buildCacheDir(), k)
	size := int64(-1)
	_ = safeRun(func() { size = treeSize(filepath.Join(dir, "data")) })
	buildCacheMu.Lock()
	defer buildCacheMu.Unlock()
	if buildCacheInUse[k]--; buildCacheInUse[k] <= 0 {
		delete(buildCacheInUse, k)
	}
	_ = safeRun(func() {
		entry := readObject(filepath.Join(dir, "entry.json"))
		if size >= 0 {
			entry["Size"] = size
		}
		entry["LastUsedAt"], entry["UsageCount"] = time.Now().UTC().Format(time.RFC3339Nano), integer(entry["UsageCount"])+1
		save(filepath.Join(dir, "entry.json"), entry)
	})
}
