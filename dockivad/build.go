package main

// docker build through Docker's classic builder API (POST /build). The Dockerfile
// runs against the build context the client sends as a tar: RUN in a temporary
// container on a clone of the stage's files, COPY and ADD straight into them. The
// image is the base image's layers plus one layer with the stage's changes.
// ponytail: no step cache (every build runs every step) and no BuildKit features
// (RUN --mount, heredocs, secrets); a per-step cache keyed by the parent stage and
// the instruction is the upgrade when rebuild time matters.

import (
	"archive/tar"
	"bufio"
	"compress/bzip2"
	"compress/gzip"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

type buildStage struct {
	name        string
	root        string // the stage's files, changed in place
	config      obj    // the image config's "config": Env, Cmd, Entrypoint, WorkingDir, ...
	digest      string // the base image's digest, recorded on build containers
	baseRoot    string // the files the new layer differs from ("" for scratch)
	baseLayout  string // the base image's OCI layout, for its layer blobs
	baseLayers  []any
	baseDiffs   []any
	baseHistory []any
	args        map[string]string // ARGs declared in the stage
	cmdSet      bool              // CMD given in this stage (ENTRYPOINT keeps it)
}

type builder struct {
	b       *engine
	ctx     context.Context
	say     func(string)
	work    string
	context string
	escape  byte
	network string
	args    map[string]string // --build-arg values
	used    map[string]bool
	global  map[string]string // ARGs declared before the first FROM
	stages  []*buildStage
}

// Builders predefine these, as BuildKit does; a Dockerfile still declares them with ARG.
var platformArgs = map[string]string{"TARGETPLATFORM": "linux/arm64", "TARGETOS": "linux", "TARGETARCH": "arm64", "TARGETVARIANT": "",
	"BUILDPLATFORM": "linux/arm64", "BUILDOS": "linux", "BUILDARCH": "arm64", "BUILDVARIANT": ""}

var buildTagPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9._/-]*(:[0-9]+/[a-z0-9._/-]+)?(:[A-Za-z0-9_][A-Za-z0-9_.-]{0,127})?$`)
var argName = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

// build serves POST /build: progress as JSON messages, as the docker CLI expects.
func (b *engine) build(w http.ResponseWriter, r *http.Request, q url.Values) {
	reject(q.Get("platform") == "" || q.Get("platform") == "linux/arm64", "Only linux/arm64 builds are supported")
	tags := []string{}
	for _, t := range q["t"] {
		tags = append(tags, normalizeTag(t))
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(200)
	var mu sync.Mutex
	enc := json.NewEncoder(w)
	send := func(m obj) {
		mu.Lock()
		defer mu.Unlock()
		_ = enc.Encode(m)
		if f, ok := w.(http.Flusher); ok {
			f.Flush()
		}
	}
	defer func() {
		if v := recover(); v != nil {
			message := buildError(v)
			send(obj{"errorDetail": obj{"message": message}, "error": message})
		}
	}()
	work, e := os.MkdirTemp(private(filepath.Join(b.Home, "build")), "b-")
	check(e)
	defer func() { go os.RemoveAll(work) }()
	quiet := q.Get("q") == "1" || q.Get("q") == "true"
	bd := &builder{b: b, ctx: r.Context(), work: work, context: filepath.Join(work, "context"), network: q.Get("networkmode"),
		args: map[string]string{}, used: map[string]bool{}, global: map[string]string{}}
	bd.say = func(s string) {
		if !quiet {
			send(obj{"stream": s})
		}
	}
	for k, v := range platformArgs {
		bd.global[k] = v
	}
	if raw := q.Get("buildargs"); raw != "" {
		for k, v := range decode([]byte(raw), maxJSON) {
			if v != nil {
				bd.args[k] = str(v)
			}
		}
	}
	labels := obj{}
	if raw := q.Get("labels"); raw != "" {
		labels = decode([]byte(raw), maxJSON)
	}
	text := bd.receiveContext(r.Body, q.Get("dockerfile"))
	id := bd.run(text, q.Get("target"), tags, labels)
	unused := []string{}
	for k := range bd.args {
		if !bd.used[k] {
			unused = append(unused, k)
		}
	}
	if len(unused) > 0 {
		sort.Strings(unused)
		bd.say("[Warning] One or more build-args " + fmt.Sprint(unused) + " were not consumed\n")
	}
	send(obj{"aux": obj{"ID": id}})
	if quiet {
		send(obj{"stream": id + "\n"})
		return
	}
	bd.say("Successfully built " + id[7:19] + "\n")
	for _, t := range tags {
		bd.say("Successfully tagged " + t + "\n")
	}
}

func buildError(v any) string {
	switch e := v.(type) {
	case apiError:
		return e.Message
	case error:
		return e.Error()
	}
	return fmt.Sprint(v)
}

// normalizeTag is a -t name as Docker stores it: lower case, ":latest" when untagged.
func normalizeTag(t string) string {
	if !strings.Contains(t, "@") && !strings.Contains(t[strings.LastIndex(t, "/")+1:], ":") {
		t += ":latest"
	}
	reject(len(t) <= 255 && buildTagPattern.MatchString(t), "invalid reference format: "+t+" (repository names are lower case)")
	return t
}

// receiveContext unpacks the build context and returns the Dockerfile. A body that
// is not a tar is the Dockerfile itself (docker build - < Dockerfile).
func (bd *builder) receiveContext(body io.Reader, dockerfile string) string {
	check(os.Mkdir(bd.context, 0o700))
	in := bufio.NewReaderSize(body, 1<<16)
	var r io.Reader = in
	if head, _ := in.Peek(3); len(head) >= 2 && head[0] == 0x1f && head[1] == 0x8b { // docker build --compress
		gz, e := gzip.NewReader(in)
		check(e)
		r = gz
	} else if string(head) == "BZh" {
		r = bzip2.NewReader(in)
	}
	br := bufio.NewReaderSize(r, 1<<16)
	if magic, _ := br.Peek(262); len(magic) < 262 || string(magic[257:262]) != "ustar" {
		data, e := io.ReadAll(io.LimitReader(br, maxJSON+1))
		check(e)
		reject(int64(len(data)) <= maxJSON, "The Dockerfile exceeds 1 MiB")
		return string(data)
	}
	extractContext(bd.context, br)
	name := dockerfile
	if name == "" {
		name = "Dockerfile"
	}
	rel := pathName(strings.TrimPrefix(name, "/"))
	if !exists(filepath.Join(bd.context, rel)) && dockerfile == "" && exists(filepath.Join(bd.context, "dockerfile")) {
		rel = "dockerfile"
	}
	if !exists(filepath.Join(bd.context, rel)) {
		panic(apiError{400, "Cannot locate specified Dockerfile: " + name})
	}
	return string(readBounded(bd.context, rel, maxJSON))
}

// extractContext writes the context tar into dir, never through a symlink.
func extractContext(dir string, r io.Reader) {
	t := tar.NewReader(r)
	members := 0
	for {
		h, e := t.Next()
		if e == io.EOF {
			return
		}
		check(e)
		members++
		reject(members <= maxMembers, "Too many files in the build context")
		rel := pathName(strings.TrimPrefix(h.Name, "./"))
		if rel == "" {
			continue
		}
		parent, last := path.Split(rel)
		dest := filepath.Join(makeParents(dir, parent), last)
		mode := tarMode(h)
		old, e := os.Lstat(dest)
		if e == nil && !(old.IsDir() && h.Typeflag == tar.TypeDir) {
			check(os.RemoveAll(dest))
		}
		switch h.Typeflag {
		case tar.TypeDir:
			if e != nil || !old.IsDir() {
				check(os.Mkdir(dest, 0o700))
			}
			check(setMode(dest, mode|0o700))
		case tar.TypeReg, tar.TypeRegA:
			f, e := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, mode&os.ModePerm|0o600)
			check(e)
			_, e = io.Copy(f, t)
			check(errors.Join(e, f.Close()))
			check(setMode(dest, mode|0o400))
			check(os.Chtimes(dest, h.ModTime, h.ModTime))
		case tar.TypeSymlink:
			check(os.Symlink(h.Linkname, dest))
		case tar.TypeLink:
			target := pathName(strings.TrimPrefix(h.Linkname, "./"))
			tparent, tbase := path.Split(target)
			check(os.Link(filepath.Join(noLinks(dir, tparent), tbase), dest))
		}
	}
}

// run executes the Dockerfile and commits the target stage (the last by default).
func (bd *builder) run(text, target string, tags []string, labels obj) string {
	instrs, escape := parseDockerfile(text)
	bd.escape = escape
	reject(len(instrs) > 0, "The Dockerfile has no instructions")
	for i, in := range instrs {
		if in.cmd == "FROM" && target != "" && bd.findStage(target) != nil {
			break // the stages after the target are not needed
		}
		if bd.ctx.Err() != nil {
			panic(apiError{499, "Build cancelled"})
		}
		bd.say(fmt.Sprintf("Step %d/%d : %s\n", i+1, len(instrs), in.original))
		bd.exec(in)
	}
	reject(len(bd.stages) > 0, "The Dockerfile has no FROM instruction")
	st := bd.stages[len(bd.stages)-1]
	if target != "" {
		st = bd.findStage(target)
		reject(st != nil, "Target stage not found: "+target)
	}
	if len(labels) > 0 {
		all := object(st.config["Labels"])
		for k, v := range labels {
			all[k] = v
		}
		st.config["Labels"] = all
	}
	return bd.commit(st, tags)
}

func (bd *builder) findStage(name string) *buildStage {
	if n, e := strconv.Atoi(name); e == nil && n >= 0 && n < len(bd.stages) {
		return bd.stages[n]
	}
	for _, st := range bd.stages {
		if st.name != "" && st.name == strings.ToLower(name) {
			return st
		}
	}
	return nil
}

func (bd *builder) exec(in instruction) {
	switch in.cmd {
	case "FROM":
		bd.from(in)
		return
	case "ARG":
		bd.arg(in)
		return
	}
	reject(len(bd.stages) > 0, in.cmd+" comes before FROM; only ARG may")
	st := bd.stages[len(bd.stages)-1]
	env := bd.env(st)
	word := func() string { return processWords(in.rest, env, bd.escape, false)[0] }
	switch in.cmd {
	case "RUN":
		bd.runStep(st, in)
	case "COPY", "ADD":
		bd.copy(st, in, env)
	case "ENV", "LABEL":
		for _, kv := range bd.pairs(in, env) {
			if in.cmd == "ENV" {
				setEnv(st.config, kv[0], kv[1])
			} else {
				labels := object(st.config["Labels"])
				labels[kv[0]] = kv[1]
				st.config["Labels"] = labels
			}
		}
	case "WORKDIR":
		wd := word()
		reject(wd != "", "WORKDIR needs a path")
		if !strings.HasPrefix(wd, "/") {
			wd = path.Join(stageWorkdir(st), wd)
		}
		wd = path.Clean(wd)
		guestDir(st.root, wd)
		st.config["WorkingDir"] = wd
	case "USER":
		u := word()
		reject(u != "", "USER needs a user")
		st.config["User"] = validUser(u)
	case "EXPOSE":
		ports := object(st.config["ExposedPorts"])
		for _, p := range strings.Fields(strings.Join(processWords(in.rest, env, bd.escape, true), " ")) { // EXPOSE $PORTS: values split too
			if !strings.Contains(p, "/") {
				p += "/tcp"
			}
			ports[strings.ToLower(p)] = obj{}
		}
		st.config["ExposedPorts"] = ports
	case "CMD", "ENTRYPOINT":
		command := bd.command(st, in)
		if in.cmd == "CMD" {
			st.config["Cmd"], st.cmdSet = command, true
		} else {
			st.config["Entrypoint"] = command
			if !st.cmdSet { // Docker: a new ENTRYPOINT drops the base image's CMD
				delete(st.config, "Cmd")
			}
		}
	case "VOLUME":
		volumes := object(st.config["Volumes"])
		list := in.json
		if list == nil {
			list = processWords(in.rest, env, bd.escape, true)
		}
		for _, v := range list {
			reject(strings.HasPrefix(v, "/"), "VOLUME must be an absolute path: "+v)
			volumes[path.Clean(v)] = obj{}
		}
		st.config["Volumes"] = volumes
	case "STOPSIGNAL":
		st.config["StopSignal"] = stopSignal(word())
	case "HEALTHCHECK":
		st.config["Healthcheck"] = healthcheckConfig(in)
	case "SHELL":
		reject(len(in.json) > 0, "SHELL needs a JSON array, for example SHELL [\"/bin/bash\", \"-c\"]")
		st.config["Shell"] = anyStrings(in.json)
	case "ONBUILD":
		st.config["OnBuild"] = append(anyList(st.config["OnBuild"]), in.rest)
	case "MAINTAINER":
		// Deprecated; Docker records it only as the image author.
	default:
		panic(apiError{400, "Unknown instruction: " + in.cmd})
	}
}

// env is what variables in a stage's instructions expand to: its ARGs, then ENV.
func (bd *builder) env(st *buildStage) map[string]string {
	env := map[string]string{}
	for k, v := range st.args {
		env[k] = v
	}
	for _, kv := range stringsOfAny(st.config["Env"]) {
		k, v, _ := strings.Cut(kv, "=")
		env[k] = v
	}
	return env
}

// pairs reads ENV and LABEL arguments: name=value pairs, or the older "name value".
func (bd *builder) pairs(in instruction, env map[string]string) [][2]string {
	words := processWords(in.rest, env, bd.escape, true)
	reject(len(words) > 0, in.cmd+" needs a name and a value")
	out := [][2]string{}
	if !strings.Contains(words[0], "=") {
		fields := regexp.MustCompile(`^(\S+)\s+(.*)$`).FindStringSubmatch(strings.TrimSpace(in.rest))
		reject(fields != nil, in.cmd+" needs a name and a value")
		return [][2]string{{processWords(fields[1], env, bd.escape, false)[0], processWords(fields[2], env, bd.escape, false)[0]}}
	}
	for _, w := range words {
		k, v, ok := strings.Cut(w, "=")
		reject(ok && k != "", in.cmd+" needs name=value: "+w)
		out = append(out, [2]string{k, v})
	}
	return out
}

func setEnv(config obj, k, v string) {
	reject(k != "" && !strings.ContainsAny(k, "= \t\n"), "Invalid environment name: "+k)
	env := []any{}
	for _, kv := range stringsOfAny(config["Env"]) {
		if name, _, _ := strings.Cut(kv, "="); name != k {
			env = append(env, kv)
		}
	}
	config["Env"] = append(env, k+"="+v)
}

func stageWorkdir(st *buildStage) string {
	if wd := text(st.config["WorkingDir"]); wd != "" {
		return wd
	}
	return "/"
}

// command is the exec form as given, or the shell form run by the stage's SHELL.
func (bd *builder) command(st *buildStage, in instruction) []any {
	if in.json != nil {
		return anyStrings(in.json)
	}
	reject(strings.TrimSpace(in.rest) != "", in.cmd+" needs a command")
	shell := stringsOfAny(st.config["Shell"])
	if len(shell) == 0 {
		shell = []string{"/bin/sh", "-c"}
	}
	return anyStrings(append(shell, in.rest))
}

func healthcheckConfig(in instruction) obj {
	rest := strings.TrimSpace(in.rest)
	if strings.EqualFold(rest, "NONE") {
		return obj{"Test": []any{"NONE"}}
	}
	kind, command, _ := strings.Cut(rest, " ")
	reject(strings.EqualFold(kind, "CMD"), "HEALTHCHECK needs NONE or CMD")
	command = strings.TrimSpace(command)
	test := []any{"CMD-SHELL", command}
	var exec []string
	if strings.HasPrefix(command, "[") && json.Unmarshal([]byte(command), &exec) == nil {
		test = append([]any{"CMD"}, anyStrings(exec)...)
	}
	hc := obj{"Test": test}
	for name, v := range in.flags {
		keys := map[string]string{"interval": "Interval", "timeout": "Timeout", "start-period": "StartPeriod", "start-interval": "StartInterval"}
		if k, ok := keys[name]; ok {
			d, e := time.ParseDuration(v)
			reject(e == nil && d >= 0, "Invalid HEALTHCHECK --"+name+": "+v)
			hc[k] = d.Nanoseconds()
			continue
		}
		n, e := strconv.Atoi(v)
		reject(name == "retries" && e == nil && n >= 0, "Unsupported HEALTHCHECK option: --"+name)
		hc["Retries"] = n
	}
	return hc
}

func (bd *builder) arg(in instruction) {
	var scope map[string]string
	if len(bd.stages) > 0 {
		scope = bd.stages[len(bd.stages)-1].args
	}
	env := bd.global
	if scope != nil {
		env = bd.env(bd.stages[len(bd.stages)-1])
	}
	for _, w := range processWords(in.rest, env, bd.escape, true) {
		name, def, hasDefault := strings.Cut(w, "=")
		reject(argName.MatchString(name), "Invalid ARG name: "+name)
		value, ok := bd.args[name]
		if ok {
			bd.used[name] = true
		} else if hasDefault {
			value, ok = def, true
		} else if scope != nil {
			value, ok = bd.global[name] // a global ARG, redeclared in the stage
		}
		switch {
		case !ok:
		case scope == nil:
			bd.global[name] = value
		default:
			scope[name] = value
		}
	}
}

func (bd *builder) from(in instruction) {
	if p := in.flags["platform"]; p != "" {
		p = processWords(p, bd.global, bd.escape, false)[0]
		reject(p == "linux/arm64" || p == "linux/arm64/v8", "Only linux/arm64 builds are supported, not "+p)
	}
	words := processWords(in.rest, bd.global, bd.escape, true)
	reject(len(words) == 1 || len(words) == 3 && strings.EqualFold(words[1], "AS"), "FROM needs an image, optionally followed by AS name")
	st := &buildStage{root: filepath.Join(bd.work, "stage-"+strconv.Itoa(len(bd.stages))), args: map[string]string{}, config: obj{}}
	if len(words) == 3 {
		st.name = strings.ToLower(words[2])
		reject(bd.findStage(st.name) == nil, "Duplicate stage name: "+words[2])
	}
	ref := words[0]
	var parent *buildStage
	for _, s := range bd.stages {
		if s.name != "" && s.name == strings.ToLower(ref) {
			parent = s
		}
	}
	switch {
	case parent != nil:
		cloneTree(parent.root, st.root)
		st.config = cloneObj(parent.config)
		st.digest, st.baseRoot, st.baseLayout = parent.digest, parent.baseRoot, parent.baseLayout
		st.baseLayers, st.baseDiffs, st.baseHistory = parent.baseLayers, parent.baseDiffs, parent.baseHistory
	case strings.EqualFold(ref, "scratch"):
		check(os.Mkdir(st.root, 0o755))
		st.digest = "sha256:" + strings.Repeat("0", 64)
	default:
		im := bd.pull(ref)
		cloneTree(im.Root, st.root)
		manifest, config := imageDocs(im.layout)
		st.config = cloneObj(object(config["config"]))
		st.digest, st.baseRoot, st.baseLayout = im.Digest, im.Root, im.layout
		st.baseLayers, st.baseDiffs = array(manifest["layers"]), array(object(config["rootfs"])["diff_ids"])
		if h, ok := config["history"].([]any); ok {
			st.baseHistory = h
		}
		bd.say(" ---> " + im.Digest[7:19] + "\n")
	}
	bd.stages = append(bd.stages, st)
}

// pull is a cached image, pulled first when missing (FROM, COPY --from).
func (bd *builder) pull(ref string) *image {
	if !strings.Contains(ref, "@") && !strings.Contains(ref[strings.LastIndex(ref, "/")+1:], ":") {
		ref += ":latest"
	}
	var im *image
	if safeRun(func() { im = bd.b.image(ref) }) == nil {
		return im
	}
	bd.say("Pulling " + ref + "\n")
	pullImage(ref, bd.b.Cache, false, "")
	return bd.b.image(ref)
}

// runStep is RUN: the command in a container on a clone of the stage's files,
// whose files become the stage's when it succeeds.
func (bd *builder) runStep(st *buildStage, in instruction) {
	for name := range in.flags {
		panic(apiError{400, "RUN --" + name + " needs BuildKit; Dockiva Engine's builder does not support it yet"})
	}
	command := bd.command(st, in)
	envNames := map[string]bool{}
	for _, kv := range stringsOfAny(st.config["Env"]) {
		k, _, _ := strings.Cut(kv, "=")
		envNames[k] = true
	}
	argEnv := []string{} // ARGs reach RUN as environment, where ENV does not set them
	for k, v := range st.args {
		if !envNames[k] {
			argEnv = append(argEnv, k+"="+v)
		}
	}
	sort.Strings(argEnv)
	im := &image{Root: st.root, Env: stringsOfAny(st.config["Env"]), WorkingDir: stageWorkdir(st), User: validUser(text(st.config["User"])), StopSignal: "SIGTERM", Digest: st.digest}
	host := obj{}
	if bd.network == "host" || bd.network == "none" {
		host["NetworkMode"] = bd.network
	}
	body := obj{"Image": "dockiva-build", "Cmd": command, "Env": anyStrings(argEnv), "HostConfig": host}
	saved := bd.saveNetworkFiles(st.root)
	name := "dockiva-build-" + token()[:12]
	p := bd.b.container(name)
	defer bd.dispose(name)
	func() {
		bd.b.Mu.Lock()
		defer bd.b.Mu.Unlock()
		bd.b.createWith(name, body, im)
		bd.b.start(name)
	}()
	bd.say(" ---> Running in " + name[len("dockiva-build-"):] + "\n")
	logs := &logReader{dir: p, want: [2]bool{true, true}, raw: true}
	defer func() {
		for _, f := range logs.files {
			if f != nil {
				f.Close()
			}
		}
	}()
	var s obj
	for {
		if out := logs.next(); len(out) > 0 {
			bd.say(string(out))
		}
		if s = bd.b.statusOf(p); s == nil || !live(s) {
			break
		}
		select {
		case <-bd.ctx.Done():
			panic(apiError{499, "Build cancelled"})
		case <-time.After(50 * time.Millisecond):
		}
	}
	if out := logs.next(); len(out) > 0 {
		bd.say(string(out))
	}
	code := "unknown"
	if s != nil && s["exit_code"] != nil {
		code = fmt.Sprint(s["exit_code"])
	}
	if code != "0" {
		detail := ""
		if s != nil && s["error"] != nil {
			detail = ": " + fmt.Sprint(s["error"])
		}
		panic(apiError{400, fmt.Sprintf("The command '%s' returned a non-zero code: %s%s", strings.Join(stringsOfAny(command), " "), code, detail)})
	}
	old := st.root + ".old"
	check(os.Rename(st.root, old))
	check(os.Rename(filepath.Join(p, "rootfs"), st.root))
	go os.RemoveAll(old)
	bd.restoreNetworkFiles(st.root, saved)
}

// The engine writes these into each container; a build keeps the stage's own.
var networkFileNames = []string{"hosts", "resolv.conf"}

func (bd *builder) saveNetworkFiles(root string) map[string]string {
	saved := map[string]string{}
	_ = safeRun(func() {
		etc := noLinks(root, "etc")
		for _, name := range networkFileNames {
			if _, e := os.Lstat(filepath.Join(etc, name)); e == nil {
				kept := filepath.Join(bd.work, "saved-"+token()[:12])
				cloneTree(filepath.Join(etc, name), kept)
				saved[name] = kept
			}
		}
	})
	return saved
}

func (bd *builder) restoreNetworkFiles(root string, saved map[string]string) {
	_ = safeRun(func() {
		etc := noLinks(root, "etc")
		for _, name := range networkFileNames {
			p := filepath.Join(etc, name)
			if e := os.Remove(p); e != nil && !os.IsNotExist(e) {
				check(e)
			}
			if kept, ok := saved[name]; ok {
				check(os.Rename(kept, p))
			}
		}
	})
}

// dispose stops and removes a build container, retrying while its supervisor settles.
func (bd *builder) dispose(name string) {
	p := bd.b.container(name)
	for i := 0; i < 100 && exists(p); i++ {
		if safeRun(func() {
			bd.b.Mu.Lock()
			defer bd.b.Mu.Unlock()
			if s := bd.b.statusOf(p); s != nil && live(s) {
				bd.b.stop(name, 0)
			}
			bd.b.remove(name)
		}) == nil {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
}

// copy is COPY and ADD: context files (or another stage's or image's, --from),
// and for ADD, URLs and local archives (unpacked).
func (bd *builder) copy(st *buildStage, in instruction, env map[string]string) {
	add := in.cmd == "ADD"
	for f := range in.flags {
		reject(f == "from" || f == "chown" || f == "chmod" || f == "link", in.cmd+" --"+f+" is not supported")
	}
	// ponytail: --chown is accepted and ignored; guest files belong to the container user.
	var args []string
	if in.json != nil {
		for _, a := range in.json {
			args = append(args, processWords(a, env, bd.escape, false)[0])
		}
	} else {
		args = processWords(in.rest, env, bd.escape, true)
	}
	reject(len(args) >= 2, in.cmd+" needs a source and a destination")
	dest := args[len(args)-1]
	toDir := strings.HasSuffix(dest, "/")
	if !strings.HasPrefix(dest, "/") {
		dest = path.Join(stageWorkdir(st), dest)
	}
	if len(args) > 2 {
		reject(toDir, "When using "+in.cmd+" with more than one source file, the destination must be a directory and end with a /")
	}
	root := bd.context
	if from := in.flags["from"]; from != "" {
		from = processWords(from, env, bd.escape, false)[0]
		if s := bd.findStage(from); s != nil {
			reject(s != st, in.cmd+" --from cannot name the current stage")
			root = s.root
		} else {
			root = bd.pull(from).Root
		}
	}
	mode, chmod := os.FileMode(0), in.flags["chmod"] != ""
	if chmod {
		n, e := strconv.ParseUint(in.flags["chmod"], 8, 32)
		reject(e == nil && n <= 0o777, "Invalid --chmod (octal permissions): "+in.flags["chmod"])
		mode = os.FileMode(n)
	}
	for _, src := range args[:len(args)-1] {
		if add && (strings.HasPrefix(src, "http://") || strings.HasPrefix(src, "https://")) {
			bd.download(st, src, dest, toDir, mode, chmod)
			continue
		}
		matches := globWithin(root, src)
		reject(len(matches) > 0, in.cmd+" failed: "+src+" not found (in the build context, or excluded by .dockerignore)")
		if len(matches) > 1 {
			reject(toDir, "When using "+in.cmd+" with more than one source file, the destination must be a directory and end with a /")
		}
		for _, m := range matches {
			info, e := os.Lstat(m.host)
			check(e)
			switch {
			case info.IsDir():
				copyIntoGuest(m.host, st.root, dest, mode, chmod)
			case add && isArchive(m.host):
				extractArchive(m.host, guestDir(st.root, dest))
			default:
				target := dest
				if toDir || isGuestDir(st.root, dest) {
					target = path.Join(dest, m.name)
				}
				copyFileTo(m.host, st.root, target, mode, chmod)
			}
		}
	}
}

type globMatch struct{ name, host string }

// globWithin finds a COPY source under root: path patterns (*, ?, [...]) per
// component, symlinks followed inside root only.
func globWithin(root, pattern string) []globMatch {
	rel := pathName(strings.TrimPrefix(path.Clean("/"+pattern), "/"))
	if rel == "" {
		return []globMatch{{".", root}}
	}
	link := func(p string) (string, bool) { return readLink(filepath.Join(root, p)) }
	found := []string{""}
	for _, part := range strings.Split(rel, "/") {
		next := []string{}
		for _, prefix := range found {
			if !strings.ContainsAny(part, "*?[") {
				next = append(next, path.Join(prefix, part))
				continue
			}
			entries, e := os.ReadDir(filepath.Join(root, resolveGuest(prefix, link)))
			if e != nil {
				continue
			}
			for _, entry := range entries {
				if ok, _ := path.Match(part, entry.Name()); ok {
					next = append(next, path.Join(prefix, entry.Name()))
				}
			}
		}
		found = next
	}
	out := []globMatch{}
	for _, g := range found {
		host := filepath.Join(root, resolveGuest(g, link))
		if _, e := os.Lstat(host); e == nil {
			out = append(out, globMatch{path.Base(g), host})
		}
	}
	return out
}

// guestRel resolves an absolute guest path in a root, following guest symlinks as
// the runtime does; the result is root-relative.
func guestRel(root, guest string) string {
	return resolveGuest(pathName(strings.TrimPrefix(path.Clean("/"+guest), "/")), func(p string) (string, bool) { return readLink(filepath.Join(root, p)) })
}

// guestDir makes a guest directory exist and returns its host path.
func guestDir(root, guest string) string {
	dir, _ := layerDir(root, guestRel(root, guest), true)
	return dir
}

func isGuestDir(root, guest string) bool {
	st, e := os.Lstat(filepath.Join(root, guestRel(root, guest)))
	return e == nil && st.IsDir()
}

// copyIntoGuest copies a directory's contents into a guest directory, merging.
func copyIntoGuest(src, root, dest string, mode os.FileMode, chmod bool) {
	guestDir(root, dest)
	check(filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		check(err)
		rel, e := filepath.Rel(src, p)
		check(e)
		if rel == "." {
			return nil
		}
		g := path.Join(dest, filepath.ToSlash(rel))
		if !d.IsDir() {
			copyFileTo(p, root, g, mode, chmod)
			return nil
		}
		created := !isGuestDir(root, g)
		h := guestDir(root, g)
		if created {
			info, e := d.Info()
			check(e)
			check(setMode(h, info.Mode()&permBits|0o700))
		}
		return nil
	}))
}

// copyFileTo copies a file or symlink to a guest path (an APFS clone where it can).
func copyFileTo(src, root, guest string, mode os.FileMode, chmod bool) {
	parent, base := path.Split(path.Clean("/" + guest))
	reject(base != "", "Cannot copy a file onto /")
	dst := filepath.Join(guestDir(root, parent), base)
	if old, e := os.Lstat(dst); e == nil {
		reject(!old.IsDir(), "Cannot replace the directory "+guest+" with a file")
		check(os.Remove(dst))
	}
	cloneTree(src, dst)
	if st, e := os.Lstat(dst); e == nil && st.Mode().IsRegular() {
		m := st.Mode() & permBits
		if chmod {
			m = mode
		}
		check(setMode(dst, m|0o400))
	}
}

// isArchive is ADD's test for a local archive to unpack: tar, gzip or bzip2.
// ponytail: xz archives are copied, not unpacked (no xz in the standard library).
func isArchive(p string) bool {
	f, e := os.Open(p)
	if e != nil {
		return false
	}
	defer f.Close()
	head := make([]byte, 262)
	n, _ := io.ReadFull(f, head)
	head = head[:n]
	return n >= 2 && head[0] == 0x1f && head[1] == 0x8b || n >= 3 && string(head[:3]) == "BZh" || n >= 262 && string(head[257:262]) == "ustar"
}

func extractArchive(p, dir string) {
	f, e := os.Open(p)
	check(e)
	defer f.Close()
	in := bufio.NewReader(f)
	var r io.Reader = in
	if head, _ := in.Peek(3); len(head) >= 2 && head[0] == 0x1f && head[1] == 0x8b {
		gz, e := gzip.NewReader(in)
		check(e)
		r = gz
	} else if string(head) == "BZh" {
		r = bzip2.NewReader(in)
	}
	members := 0
	applyLayer(dir, r, &members)
}

// download is ADD of a URL: the file, mode 0600 unless --chmod, dated Last-Modified.
func (bd *builder) download(st *buildStage, rawURL, dest string, toDir bool, mode os.FileMode, chmod bool) {
	u, e := url.Parse(rawURL)
	reject(e == nil && u.Host != "", "Invalid ADD URL: "+rawURL)
	target := dest
	if toDir || isGuestDir(st.root, dest) {
		name := path.Base(u.Path)
		reject(name != "" && name != "/" && name != ".", "ADD "+rawURL+": give a destination file name")
		target = path.Join(dest, name)
	}
	bd.say("Downloading " + rawURL + "\n")
	req, e := http.NewRequestWithContext(bd.ctx, "GET", rawURL, nil)
	check(e)
	resp, e := (&http.Client{Timeout: 30 * time.Minute}).Do(req)
	reject(e == nil, fmt.Sprintf("ADD %s: %v", rawURL, e))
	defer resp.Body.Close()
	reject(resp.StatusCode == 200, fmt.Sprintf("ADD %s: HTTP %d", rawURL, resp.StatusCode))
	parent, base := path.Split(path.Clean("/" + target))
	dst := filepath.Join(guestDir(st.root, parent), base)
	if old, e := os.Lstat(dst); e == nil {
		reject(!old.IsDir(), "Cannot replace the directory "+target+" with a file")
		check(os.Remove(dst))
	}
	f, e := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, 0o600)
	check(e)
	_, e = io.Copy(f, resp.Body)
	check(errors.Join(e, f.Close()))
	if chmod {
		check(os.Chmod(dst, mode|0o400))
	}
	if t, e := http.ParseTime(resp.Header.Get("Last-Modified")); e == nil {
		_ = os.Chtimes(dst, t, t)
	}
}

// commit writes the stage as an image (base layers plus one layer of changes) and
// caches it under each tag; untagged, under "<none>:ID".
func (bd *builder) commit(st *buildStage, tags []string) string {
	out := filepath.Join(bd.work, "out")
	layout := filepath.Join(out, "layout")
	blobs := filepath.Join(layout, "blobs", "sha256")
	check(os.MkdirAll(blobs, 0o700))
	layers := append([]any{}, st.baseLayers...)
	diffs := append([]any{}, st.baseDiffs...)
	history := append([]any{}, st.baseHistory...)
	for _, l := range layers {
		d := str(object(l)["digest"])
		if !exists(filepath.Join(blobs, d[7:])) {
			linkOrCopy(noLinks(st.baseLayout, "blobs/sha256/"+d[7:]), filepath.Join(blobs, d[7:]))
		}
	}
	d, size, changed := writeDiffLayer(st.root, st.baseRoot, blobs)
	created := time.Now().UTC().Format(time.RFC3339Nano)
	if changed || len(layers) == 0 {
		layers = append(layers, obj{"mediaType": layerMedia, "digest": d, "size": size})
		diffs = append(diffs, d)
		history = append(history, obj{"created": created, "created_by": "dockiva-engine build"})
	} else {
		history = append(history, obj{"created": created, "created_by": "dockiva-engine build", "empty_layer": true})
	}
	config := obj{"architecture": "arm64", "os": "linux", "created": created, "config": st.config,
		"rootfs": obj{"type": "layers", "diff_ids": diffs}, "history": history}
	configDigest, configSize := writeJSONBlob(blobs, config)
	manifest := obj{"schemaVersion": 2, "mediaType": manifestMedia, "config": obj{"mediaType": configMedia, "digest": configDigest, "size": configSize}, "layers": layers}
	id, manifestSize := writeJSONBlob(blobs, manifest)
	save(filepath.Join(layout, "oci-layout"), obj{"imageLayoutVersion": "1.0.0"})
	save(filepath.Join(layout, "index.json"), obj{"schemaVersion": 2, "manifests": []any{obj{"mediaType": manifestMedia, "digest": id, "size": manifestSize, "platform": obj{"architecture": "arm64", "os": "linux"}}}})
	loadImage(layout) // the checks every cached image passes
	check(os.Rename(st.root, filepath.Join(out, "rootfs")))
	save(filepath.Join(out, "image.json"), obj{"name": "", "digest": id, "size": treeSize(filepath.Join(out, "rootfs"))})
	if len(tags) == 0 {
		tags = []string{"<none>:" + id[7:19]}
	}
	for _, t := range tags {
		bd.b.commitTag(shortName(t), id, layout)
	}
	return id
}

func writeJSONBlob(blobs string, v any) (string, int) {
	raw, e := json.Marshal(v)
	check(e)
	d := digest(raw)
	check(os.WriteFile(filepath.Join(blobs, d[7:]), raw, 0o600))
	return d, len(raw)
}

func cloneObj(o obj) obj {
	raw, e := json.Marshal(o)
	check(e)
	return decode(raw, maxJSON)
}

func anyStrings(s []string) []any {
	out := make([]any, len(s))
	for i, v := range s {
		out[i] = v
	}
	return out
}

func anyList(v any) []any {
	if a, ok := v.([]any); ok {
		return a
	}
	return []any{}
}

// stringsOfAny is stringsOf without its 64-entry limit, for image settings.
func stringsOfAny(v any) []string {
	out := []string{}
	for _, s := range anyList(v) {
		out = append(out, str(s))
	}
	return out
}
