package main

// Build sources beyond a local directory: a Git repository or an http(s) URL as the
// build context (docker build https://...), and named contexts (docker build
// --build-context name=...), which FROM, COPY --from and RUN --mount from= find
// before stages and images: a local directory, an image (docker-image://), or a
// Git or http(s) source. And ADD's archives beyond gzip and bzip2: xz and zstd.

import (
	"bufio"
	"bytes"
	"compress/bzip2"
	"compress/gzip"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"
	"time"

	"github.com/klauspost/compress/zstd"
)

// namedContext is a --build-context: a directory (local or fetched), or an image.
type namedContext struct {
	dir   string
	image string
}

var gitURLSuffix = regexp.MustCompile(`\.git(?:#.+)?$`)

// isGitContext is a context the docker CLI and BuildKit read as a Git repository:
// git://, git@host:, ssh://, github.com/, or an http(s) URL ending in .git, each
// with an optional #ref:subdir.
func isGitContext(s string) bool {
	return strings.HasPrefix(s, "git://") || strings.HasPrefix(s, "git@") || strings.HasPrefix(s, "ssh://") ||
		strings.HasPrefix(s, "github.com/") || isURL(s) && gitURLSuffix.MatchString(s)
}

// fetchRemote writes a Git repository or an http(s) URL into dir (new) and returns
// the directory the build uses: the repository's #ref:subdir, or dir.
func (bd *builder) fetchRemote(spec, dir string) string {
	if isGitContext(spec) {
		return bd.cloneGit(spec, dir)
	}
	reject(isURL(spec), "Unsupported build context "+spec+": a local directory, a Git repository or an http(s) URL")
	check(os.Mkdir(dir, 0o700))
	f := filepath.Join(bd.work, "remote-"+token()[:12])
	bd.fetchURL(spec, f)
	defer os.Remove(f)
	if r, ok := archiveStream(f); ok { // a tar archive: the context
		defer r.Close()
		extractContext(dir, r)
		return dir
	}
	check(os.Rename(f, filepath.Join(dir, "Dockerfile"))) // otherwise the Dockerfile, with an empty context
	return dir
}

// cloneGit checks out spec's ref (the default branch when none) with the Mac's git,
// without its .git directory, as BuildKit's Git source does, and returns its
// #ref:subdir. Only network protocols: no file:// (the Mac's files) or ext::.
func (bd *builder) cloneGit(spec, dir string) string {
	git, e := exec.LookPath("git")
	reject(e == nil, "A Git build context needs git on the Mac; install the Xcode Command Line Tools (xcode-select --install)")
	remote, fragment, _ := strings.Cut(spec, "#")
	if strings.HasPrefix(remote, "github.com/") {
		remote = "https://" + remote
	}
	ref, sub, _ := strings.Cut(fragment, ":")
	check(os.Mkdir(dir, 0o700))
	run := func(args ...string) {
		cmd := exec.CommandContext(bd.ctx, git, append([]string{"-c", "protocol.file.allow=never", "-c", "core.hooksPath=/dev/null",
			"-c", "advice.detachedHead=false"}, args...)...)
		cmd.Dir = dir
		cmd.Env = append(os.Environ(), "GIT_TERMINAL_PROMPT=0", "GIT_ALLOW_PROTOCOL=https:http:ssh:git", "GIT_LFS_SKIP_SMUDGE=1")
		out, e := cmd.CombinedOutput()
		reject(e == nil, fmt.Sprintf("git %s %s failed: %s", args[0], remote, strings.TrimSpace(string(out))))
	}
	if ref == "" {
		ref = "HEAD"
	}
	bd.say("Cloning " + remote + " (" + ref + ")\n")
	run("init", "-q")
	run("remote", "add", "origin", remote)
	run("fetch", "-q", "--depth", "1", "--no-tags", "origin", ref) // a branch, a tag or a full commit hash
	run("checkout", "-q", "FETCH_HEAD")
	if exists(filepath.Join(dir, ".gitmodules")) {
		run("submodule", "update", "-q", "--init", "--recursive", "--depth", "1")
	}
	check(os.RemoveAll(filepath.Join(dir, ".git")))
	if sub = pathName(strings.Trim(sub, "/")); sub == "" {
		return dir
	}
	parent, base := path.Split(sub)
	sd := filepath.Join(noLinks(dir, parent), base)
	info, e := os.Lstat(sd)
	reject(e == nil && info.IsDir(), "The Git context "+remote+" has no directory "+sub)
	return sd
}

// fetchURL downloads rawURL to the new file dest.
func (bd *builder) fetchURL(rawURL, dest string) {
	u, e := url.Parse(rawURL)
	reject(e == nil && u.Host != "", "Invalid URL: "+rawURL)
	bd.say("Downloading " + rawURL + "\n")
	req, e := http.NewRequestWithContext(bd.ctx, "GET", rawURL, nil)
	check(e)
	resp, e := (&http.Client{Timeout: 30 * time.Minute}).Do(req)
	reject(e == nil, fmt.Sprintf("%s: %v", rawURL, e))
	defer resp.Body.Close()
	reject(resp.StatusCode == 200, fmt.Sprintf("%s: HTTP %d", rawURL, resp.StatusCode))
	f, e := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW, 0o600)
	check(e)
	_, e = io.Copy(f, resp.Body)
	check(errors.Join(e, f.Close()))
}

// namedContexts reads buildx's named contexts from the frontend options
// ("context:NAME" = "local:DIR", "docker-image://REF", or a Git or http(s) source)
// and fetches each directory. fetchLocal fetches a session directory with its
// .dockerignore applied.
func (bd *builder) namedContexts(opts map[string]string, fetchLocal func(local, dest string)) {
	for k, spec := range opts {
		name, ok := strings.CutPrefix(k, "context:")
		if !ok {
			continue
		}
		name = strings.ToLower(name)
		dir := filepath.Join(bd.work, "named-"+token()[:12])
		switch {
		case strings.HasPrefix(spec, "docker-image://"):
			ref := strings.TrimPrefix(spec, "docker-image://")
			reject(ref != "", "--build-context "+name+"=docker-image:// needs an image")
			bd.named[name] = namedContext{image: ref}
		case strings.HasPrefix(spec, "local:"):
			fetchLocal(strings.TrimPrefix(spec, "local:"), dir)
			bd.named[name] = namedContext{dir: dir}
		case isGitContext(spec) || isURL(spec):
			bd.named[name] = namedContext{dir: bd.fetchRemote(spec, dir)}
		default: // input: (a build target, an OCI layout) is an LLB state
			panic(apiError{400, "--build-context " + name + " is not supported: Dockiva Engine's builder takes a local directory, docker-image://, a Git repository or an http(s) URL, not a build target or an OCI layout"})
		}
	}
}

// namedRoot is the files of a named context (an image's files after a pull), and
// what a build cache key records for them; ok is false for no such context.
func (bd *builder) namedRoot(name string) (root, cacheID string, ok bool) {
	n, ok := bd.named[strings.ToLower(name)]
	if !ok {
		return "", "", false
	}
	if n.image != "" {
		im := bd.pull(n.image, bd.platform)
		return im.Root, im.Digest, true
	}
	return n.dir, "context " + treeHash(n.dir), true
}

// archiveStream is the tar stream of an archive ADD unpacks (plain, gzip, bzip2, xz
// or zstd), or false for any other file. xz goes through the Mac's bsdtar (liblzma),
// as Go's standard library has none.
func archiveStream(p string) (io.ReadCloser, bool) {
	f, e := os.Open(p)
	if e != nil {
		return nil, false
	}
	head := make([]byte, 6)
	n, _ := io.ReadFull(f, head)
	head = head[:n]
	if _, e := f.Seek(0, io.SeekStart); e != nil {
		f.Close()
		return nil, false
	}
	in := bufio.NewReader(f)
	var r io.Reader = in
	closer := func() error { return f.Close() }
	switch {
	case n >= 2 && head[0] == 0x1f && head[1] == 0x8b:
		gz, e := gzip.NewReader(in)
		if e != nil {
			f.Close()
			return nil, false
		}
		r = gz
	case n >= 3 && string(head[:3]) == "BZh":
		r = bzip2.NewReader(in)
	case n >= 4 && bytes.Equal(head[:4], []byte{0x28, 0xb5, 0x2f, 0xfd}):
		z, e := zstd.NewReader(in)
		if e != nil {
			f.Close()
			return nil, false
		}
		r, closer = z, func() error { z.Close(); return f.Close() }
	case n >= 6 && bytes.Equal(head, []byte{0xfd, '7', 'z', 'X', 'Z', 0}):
		f.Close()
		cmd := exec.Command("/usr/bin/tar", "-cf", "-", "@"+p) // the archive's members as a plain tar
		cmd.Env = append(os.Environ(), "COPYFILE_DISABLE=1")
		out, e := cmd.StdoutPipe()
		if e != nil || cmd.Start() != nil {
			return nil, false
		}
		r, closer = out, func() error { out.Close(); return cmd.Wait() }
	}
	br := bufio.NewReaderSize(r, 1<<16)
	if magic, _ := br.Peek(262); len(magic) < 262 || string(magic[257:262]) != "ustar" {
		closer()
		return nil, false
	}
	return readCloser{br, closer}, true
}

type readCloser struct {
	io.Reader
	close func() error
}

func (r readCloser) Close() error { return r.close() }
