package main

// A buildx build (buildkit.go) on the classic builder: the Dockerfile and context
// come from the client's session (FileSync.DiffCopy), the builder's output becomes
// BuildKit progress, and the image is tagged with the names buildx exports.

import (
	"archive/tar"
	"context"
	"io"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"
	"time"
)

var stepLine = regexp.MustCompile(`(?s)^Step (\d+/\d+) : (.*)$`)

// bkBuild runs the Dockerfile frontend's options (FrontendOpt) and returns the image ID.
func (b *engine) bkBuild(ctx context.Context, st *bkSolve, opts map[string]string) string {
	reject(opts["requestid"] == "", "docker build --call, --check and --print need BuildKit; Dockiva Engine's builder does not support them")
	reject(opts["context"] == "", "Dockiva Engine builds from a local directory only, not a Git, URL or stdin context")
	for k := range opts {
		reject(!strings.HasPrefix(k, "context:") && !strings.HasPrefix(k, "input:"), "Named build contexts (--build-context) need BuildKit; Dockiva Engine's builder does not support them")
	}
	for _, p := range strings.Split(opts["platform"], ",") {
		reject(p == "" || p == "linux/arm64" || p == "linux/arm64/v8", "Only linux/arm64 builds are supported, not "+p)
	}
	// --pull needs nothing: FROM pulls only missing images. buildx sends "no-cache"
	// (empty for every stage) for --no-cache.
	_, nocache := opts["no-cache"]
	tags := []string{}
	for _, t := range st.names {
		tags = append(tags, normalizeTag(t))
	}
	work, e := os.MkdirTemp(private(filepath.Join(b.Home, "build")), "b-")
	check(e)
	defer func() { go os.RemoveAll(work) }()
	bd := &builder{b: b, ctx: ctx, work: work, context: filepath.Join(work, "context"), network: opts["force-network-mode"],
		args: map[string]string{}, used: map[string]bool{}, global: map[string]string{}, nocache: nocache}
	for k, v := range platformArgs {
		bd.global[k] = v
	}
	labels := obj{}
	for k, v := range opts {
		if name, ok := strings.CutPrefix(k, "build-arg:"); ok {
			bd.args[name] = v
		} else if name, ok := strings.CutPrefix(k, "label:"); ok {
			labels[name] = v
		}
	}
	key := func(name string) string { // a local directory's name in the session ("dockerfilekey")
		if k := opts[name+"key"]; k != "" {
			return k
		}
		return name
	}
	fetch := func(local, dest string, include, exclude, follow []string) int64 {
		id := opts["local-sessionid:"+local] // buildx shares a directory across builds in one session
		if id == "" {
			id = st.session
		}
		check(os.Mkdir(dest, 0o700))
		return bkSessionFor(ctx, id).fetch(ctx, local, dest, include, exclude, follow)
	}
	p := st.progress
	step := func(name string, fn func(v *bkVertex)) {
		v := p.vertex(name)
		defer func() {
			if r := recover(); r != nil {
				v.done(buildError(r))
				panic(r)
			}
		}()
		fn(v)
		v.done("")
	}

	filename := opts["filename"]
	if filename == "" {
		filename = "Dockerfile"
	}
	filename = pathName(filename)
	dockerfiles := filepath.Join(work, "dockerfile")
	var text string
	step("[internal] load build definition from "+path.Base(filename), func(v *bkVertex) {
		n := fetch(key("dockerfile"), dockerfiles, []string{filename, filename + ".dockerignore"}, nil, []string{filename})
		reject(exists(filepath.Join(dockerfiles, filename)), "Cannot locate specified Dockerfile: "+filename)
		text = string(readBounded(dockerfiles, filename, maxJSON))
		v.status("transferring dockerfile:", n)
	})
	var ignore []byte
	step("[internal] load .dockerignore", func(v *bkVertex) {
		if exists(filepath.Join(dockerfiles, filename+".dockerignore")) {
			ignore = readBounded(dockerfiles, filename+".dockerignore", maxJSON)
			return
		}
		dir := filepath.Join(work, "dockerignore")
		v.status("transferring context:", fetch(key("context"), dir, []string{".dockerignore"}, nil, []string{".dockerignore"}))
		if exists(filepath.Join(dir, ".dockerignore")) {
			ignore = readBounded(dir, ".dockerignore", maxJSON)
		}
	})
	step("[internal] load build context", func(v *bkVertex) {
		v.status("transferring context:", fetch(key("context"), bd.context, nil, dockerignorePatterns(ignore), nil))
	})

	// The builder's output: "Step 2/5 : RUN ..." starts a step; the rest is its log.
	var cur *bkVertex
	bd.say = func(s string) {
		if m := stepLine.FindStringSubmatch(s); m != nil {
			if cur != nil {
				cur.done("")
			}
			cur = p.vertex("[" + m[1] + "] " + strings.Join(strings.Fields(m[2]), " "))
			return
		}
		if cur == nil {
			cur = p.vertex("[build]")
		}
		if strings.HasPrefix(s, " ---> ") { // classic builder detail: image IDs, containers, cache hits
			cur.cached = cur.cached || strings.HasPrefix(s, " ---> Using cache")
			return
		}
		cur.log(s)
	}
	var id string
	func() {
		defer func() {
			if r := recover(); r != nil {
				if cur != nil {
					cur.done(buildError(r))
				}
				panic(r)
			}
		}()
		id = bd.run(text, opts["target"], tags, labels)
		if cur != nil {
			cur.done("")
		}
	}()
	step("exporting to image", func(v *bkVertex) {
		v.status("writing image "+id, 0)
		for _, t := range tags {
			v.status("naming to "+t, 0)
		}
	})
	return id
}

// dockerignorePatterns reads a .dockerignore as Docker does (moby/patternmatcher's
// ignorefile): comments and blank lines dropped, paths cleaned, "!" exceptions kept.
func dockerignorePatterns(data []byte) []string {
	var out []string
	for i, line := range strings.Split(string(data), "\n") {
		if i == 0 {
			line = strings.TrimPrefix(line, "\ufeff")
		}
		line = strings.TrimSpace(line)
		if line == "" || line[0] == '#' {
			continue
		}
		invert := line[0] == '!'
		if invert {
			line = strings.TrimSpace(line[1:])
		}
		if line != "" {
			line = path.Clean(line)
			if len(line) > 1 && line[0] == '/' {
				line = line[1:]
			}
		}
		if invert {
			line = "!" + line
		}
		out = append(out, line)
	}
	return out
}

// fsutil's FileSync packet types (fsutil/types/wire.proto).
const packetStat, packetReq, packetData, packetFin, packetErr = 0, 1, 2, 3, 4

// fetch copies a directory the client shares (a buildx "local", such as "context")
// into dest with FileSync.DiffCopy, and returns the bytes received. The client
// sends a listing, then each file the engine requests. The listing goes through
// extractContext as a tar of directories, links and empty files, so its path checks
// apply; file data then fills those files.
func (s *bkSession) fetch(ctx context.Context, name, dest string, include, exclude, follow []string) int64 {
	md := http.Header{}
	md.Set("dir-name", name)
	for k, values := range map[string][]string{"include-patterns": include, "exclude-patterns": exclude, "followpaths": follow} {
		if len(values) > 0 {
			for _, v := range values {
				md.Add(k, url.QueryEscape(v))
			}
			md.Set(k+"-encoded", "1")
		}
	}
	c := grpcOpen(ctx, s.rt, "/moby.filesync.v1.FileSync/DiffCopy", md)
	defer c.close()
	next := func() pbFields {
		p, ok := c.recv()
		reject(ok, "The build context transfer ended early")
		if p.num(1) == packetErr {
			panic(apiError{400, "Sending the build context failed: " + p.str(4)})
		}
		return p
	}

	type file struct {
		name  string
		mode  os.FileMode
		mtime time.Time
	}
	files := map[uint32]file{}
	pr, pw := io.Pipe()
	extracted := make(chan error, 1)
	go func() {
		e := safeRun(func() { extractContext(dest, pr) })
		pr.CloseWithError(e)
		extracted <- e
	}()
	tw := tar.NewWriter(pw)
	var werr error
	for id := uint32(0); ; id++ {
		p := next()
		reject(p.num(1) == packetStat, "Unexpected FileSync packet")
		if len(p[2]) == 0 { // an empty stat ends the listing
			break
		}
		st := p.sub(2)
		mode := os.FileMode(st.num(2)) // fsutil sends Go's FileMode bits
		h := &tar.Header{Name: st.str(1), Mode: int64(mode.Perm()), ModTime: time.Unix(0, int64(st.num(6)))}
		switch {
		case mode.IsDir():
			h.Typeflag = tar.TypeDir
		case mode&os.ModeSymlink != 0:
			h.Typeflag, h.Linkname = tar.TypeSymlink, st.str(7)
		case mode&os.ModeType != 0: // devices, pipes, sockets
			continue
		case st.str(7) != "": // a hard link to an earlier file
			h.Typeflag, h.Linkname = tar.TypeLink, st.str(7)
		default:
			files[id] = file{st.str(1), mode.Perm(), h.ModTime}
			h.Typeflag, h.Mode = tar.TypeReg, 0o600 // writable until its data arrives
		}
		if werr == nil {
			werr = tw.WriteHeader(h)
		}
	}
	if werr == nil {
		werr = tw.Close()
	}
	pw.CloseWithError(werr)
	check(<-extracted)
	check(werr)

	ids := make([]uint32, 0, len(files))
	for id := range files {
		ids = append(ids, id)
	}
	go func() { // requests go out while data comes in, or both sides could block
		for _, id := range ids {
			if c.send(pbuf{}.num(1, packetReq).num(3, uint64(id))) != nil {
				return
			}
		}
	}()
	open := map[uint32]*os.File{}
	defer func() {
		for _, f := range open {
			f.Close()
		}
	}()
	var total int64
	for len(files) > 0 {
		p := next()
		if p.num(1) != packetData {
			continue
		}
		id := uint32(p.num(3))
		meta, ok := files[id]
		reject(ok, "FileSync sent data for an unknown file")
		f := open[id]
		if f == nil {
			parent, base := path.Split(meta.name)
			var e error
			f, e = os.OpenFile(filepath.Join(noLinks(dest, parent), base), os.O_WRONLY|os.O_TRUNC|syscall.O_NOFOLLOW, 0)
			check(e)
			open[id] = f
		}
		data := p.last(4).b
		if len(data) > 0 { // an empty data packet ends the file
			_, e := f.Write(data)
			check(e)
			total += int64(len(data))
			continue
		}
		tv := syscall.NsecToTimeval(meta.mtime.UnixNano())
		check(f.Chmod(meta.mode | 0o400))
		check(syscall.Futimes(int(f.Fd()), []syscall.Timeval{tv, tv}))
		check(f.Close())
		delete(open, id)
		delete(files, id)
	}
	check(c.send(pbuf{}.num(1, packetFin)))
	for p := next(); p.num(1) != packetFin; p = next() {
	}
	c.closeSend()
	for _, more := c.recv(); more; _, more = c.recv() {
	}
	return total
}
