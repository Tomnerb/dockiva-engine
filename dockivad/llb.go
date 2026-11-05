package main

// The few LLB definitions the classic builder evaluates through the gateway
// (LLBBridge.Solve without a frontend), with StatFile and ReadFile on the result:
// Git and http(s) sources, and a FileOp copying (and unpacking) one into an empty
// tree. buildx solves exactly these to read a remote context's files before the
// build (its Dockerfile.rego policy); a Dockerfile build itself runs through
// bkBuild. Anything else is refused.

import (
	"context"
	"io"
	"os"
	"path"
	"path/filepath"
	"strings"
	"syscall"
)

// llbCaps are the LLB capabilities the gateway answers Ping with: what llbEval
// supports. Git's auth secrets and SSH options are accepted; the Mac's git uses the
// user's own credentials and keys.
var llbCaps = []string{"source.git", "source.git.fullurl", "source.git.keepgitdir", "source.git.subdir", "source.git.httpauth",
	"source.git.knownsshhosts", "source.git.mountsshsock", "source.http", "file.base",
	"constraints", "platform", "meta.description", "meta.ignorecache", "meta.exportcache"} // metadata, ignored

// llbEval evaluates a pb.Definition into a new directory under work and returns it.
func (bd *builder) llbEval(def pbFields) string {
	ops := map[string]pbFields{}
	last := ""
	for _, raw := range def[1] {
		last = digest(raw.b)
		ops[last] = pbDecode(raw.b)
	}
	reject(last != "", "An empty LLB definition")
	results := map[string]string{}
	var eval func(d string) string
	eval = func(d string) string {
		if dir, ok := results[d]; ok {
			return dir
		}
		op, ok := ops[d]
		reject(ok, "An LLB definition names an unknown operation")
		var inputs []string
		for _, in := range op[1] {
			i := pbDecode(in.b)
			reject(i.num(2) == 0, "LLB operations with more than one output are not supported")
			inputs = append(inputs, eval(i.str(1)))
		}
		dir := filepath.Join(bd.work, "llb-"+token()[:12])
		switch {
		case len(op[3]) > 0:
			dir = bd.llbSource(op.sub(3), dir)
		case len(op[4]) > 0:
			bd.llbFile(op.sub(4), inputs, dir)
		default:
			panic(apiError{400, "Dockiva Engine's builder evaluates Git and HTTP sources and file copies only, not this LLB operation"})
		}
		results[d] = dir
		return dir
	}
	terminal := ops[last]
	reject(len(terminal[1]) == 1 && len(terminal[3]) == 0 && len(terminal[4]) == 0, "An LLB definition without a result")
	return eval(pbDecode(terminal[1][0].b).str(1))
}

// llbSource fetches a SourceOp (identifier, attrs) into dir and returns the tree.
func (bd *builder) llbSource(src pbFields, dir string) string {
	id, attrs := src.str(1), src.strMap(2)
	switch {
	case strings.HasPrefix(id, "git://"):
		remote := attrs["git.fullurl"]
		rest, fragment, _ := strings.Cut(strings.TrimPrefix(id, "git://"), "#")
		if remote == "" {
			remote = "https://" + rest
		}
		spec := remote
		if fragment != "" {
			spec += "#" + fragment
		}
		return bd.cloneGit(spec, dir)
	case isURL(id):
		check(os.Mkdir(dir, 0o700))
		name := attrs["http.filename"]
		if name == "" {
			name = path.Base(strings.SplitN(strings.SplitN(id, "?", 2)[0], "#", 2)[0])
		}
		name = pathName(name)
		reject(name != "" && !strings.Contains(name, "/"), "An HTTP source needs a file name")
		bd.fetchURL(id, filepath.Join(dir, name))
		return dir
	}
	panic(apiError{400, "Dockiva Engine's builder fetches Git and http(s) sources only, not " + id})
}

// llbFile runs a FileOp of copies into an empty tree (the copy's source being one of
// the inputs), unpacking archives when asked.
func (bd *builder) llbFile(file pbFields, inputs []string, dir string) {
	check(os.Mkdir(dir, 0o700))
	for _, raw := range file[2] {
		a := pbDecode(raw.b)
		reject(int64(a.num(1)) == -1 && len(a[4]) > 0, "Dockiva Engine's builder runs file copies into an empty tree only")
		from := int(a.num(2))
		reject(from >= 0 && from < len(inputs), "A file copy's source is not an input")
		c := a.sub(4)
		src, dest := path.Clean("/"+c.str(1)), path.Clean("/"+c.str(2))
		host := noLinksOrMissing(inputs[from], strings.TrimPrefix(src, "/"))
		info, e := os.Lstat(host)
		reject(e == nil, "A file copy's source "+src+" does not exist")
		target := dir
		if dest != "/" {
			target = makeParents(dir, strings.TrimPrefix(dest, "/"))
		}
		switch {
		case info.IsDir():
			cloneTree(host, filepath.Join(target, filepath.Base(host)))
		case c.num(7) != 0 && isArchive(host): // attemptUnpackDockerCompatibility
			r, _ := archiveStream(host)
			extractContext(target, r)
			r.Close()
		default:
			linkOrCopy(host, filepath.Join(target, filepath.Base(host)))
		}
	}
}

// llbStat is StatFile: the path's fsutil Stat in a solved tree.
func llbStat(root, p string) pbuf {
	host := llbPath(root, p)
	info, e := os.Lstat(host)
	if e != nil {
		panic(grpcError{5, "stat " + p + ": no such file or directory"})
	}
	st := pbuf{}.str(1, path.Base(path.Clean("/"+p))).num(2, uint64(info.Mode())).num(5, uint64(info.Size())).num(6, uint64(info.ModTime().UnixNano()))
	if info.Mode()&os.ModeSymlink != 0 {
		target, _ := os.Readlink(host)
		st = st.str(7, target)
	}
	return pbuf{}.msg(1, st)
}

// llbRead is ReadFile: the file's bytes, or a range of them (offset, length).
func llbRead(root, p string, offset, length int64) pbuf {
	f, e := os.OpenFile(llbPath(root, p), os.O_RDONLY|syscall.O_NOFOLLOW, 0) // a link is not followed out of the tree
	if e != nil {
		panic(grpcError{5, "open " + p + ": no such file or directory"})
	}
	defer f.Close()
	_, e = f.Seek(offset, io.SeekStart)
	check(e)
	var r io.Reader = f
	if length > 0 {
		r = io.LimitReader(f, length)
	}
	data, e := io.ReadAll(io.LimitReader(r, maxJSON*16))
	check(e)
	return pbuf{}.bytes(1, data)
}

// llbPath is p inside root, never through a symbolic link out of it.
func llbPath(root, p string) string {
	rel := pathName(strings.TrimPrefix(path.Clean("/"+p), "/"))
	if rel == "" {
		return root
	}
	parent, base := path.Split(rel)
	var dir string
	if safeRun(func() { dir = noLinks(root, parent) }) != nil {
		return filepath.Join(root, ".missing-"+token())
	}
	return filepath.Join(dir, base)
}

// llbRefs are the trees solved for a build's gateway (its Control.Solve ref), by id.
type llbRefs struct {
	dirs map[string]string
	work string
}

func (s *bkSolve) llbRef(id string) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.llb.dirs == nil || s.llb.dirs[id] == "" {
		panic(grpcError{5, "result " + id + " not found"})
	}
	return s.llb.dirs[id]
}

// llbSolve is LLBBridge.Solve of a definition (no frontend): its tree, kept as a
// result reference until the build's record goes.
func (b *engine) llbSolve(ctx context.Context, st *bkSolve, def pbFields) pbuf {
	st.mu.Lock()
	if st.llb.work == "" {
		w, e := os.MkdirTemp(private(filepath.Join(b.Home, "build")), "llb-")
		if e != nil {
			st.mu.Unlock()
			check(e)
		}
		st.llb.work, st.llb.dirs = w, map[string]string{}
	}
	work := st.llb.work
	st.mu.Unlock()
	bd := &builder{b: b, ctx: ctx, work: work, say: func(string) {}}
	dir := bd.llbEval(def)
	id := "llb-" + token()[:16]
	st.mu.Lock()
	st.llb.dirs[id] = dir
	st.mu.Unlock()
	return pbuf{}.msg(3, pbuf{}.msg(3, pbuf{}.str(1, id))) // Result{ref: Ref{id}}
}
