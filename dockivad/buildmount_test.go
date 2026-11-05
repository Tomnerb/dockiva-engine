package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestParseMount(t *testing.T) {
	env := map[string]string{"ID": "npm", "DIR": "/root/.npm"}
	for _, c := range []struct {
		spec string
		want runMount
	}{
		{"type=cache,target=$DIR", runMount{kind: "cache", target: "/root/.npm", id: "/root/.npm", mode: -1}},
		{"type=cache,id=${ID},target=cache,sharing=locked,mode=0700,uid=1000,ro", runMount{kind: "cache", target: "/app/cache", id: "npm", sharing: "locked", mode: 0o700, readonly: true}},
		{"type=cache,target=/c,from=base,source=/seed", runMount{kind: "cache", target: "/c", id: "/c", from: "base", source: "/seed", mode: -1}},
		{"target=.", runMount{kind: "bind", target: "/app", readonly: true, mode: -1}},
		{"type=bind,src=go.sum,dst=/src/go.sum,from=builder,rw", runMount{kind: "bind", target: "/src/go.sum", source: "go.sum", from: "builder", mode: -1}},
		{"type=bind,target=/x,readwrite=false", runMount{kind: "bind", target: "/x", readonly: true, mode: -1}},
		{"type=tmpfs,target=/tmp/x,size=64m", runMount{kind: "tmpfs", target: "/tmp/x", mode: -1}},
		{"type=secret,id=tok", runMount{kind: "secret", id: "tok", target: "/run/secrets/tok", readonly: true, mode: -1}},
		{"type=secret,source=aws,target=/root/.aws/credentials,required,mode=0440", runMount{kind: "secret", id: "aws", target: "/root/.aws/credentials", readonly: true, required: true, mode: 0o440}},
		{"type=secret,target=/etc/token", runMount{kind: "secret", id: "token", target: "/etc/token", readonly: true, mode: -1}},
		{"type=secret,id=tok,env=TOKEN", runMount{kind: "secret", id: "tok", env: "TOKEN", readonly: true, mode: -1}},
		{`"type=bind","target=/a,b"`, runMount{kind: "bind", target: "/a,b", readonly: true, mode: -1}},
		{"type=ssh", runMount{kind: "ssh", id: "default", mode: -1}}, // runOptions numbers its target
		{"type=ssh,id=gh,target=/agent.sock,required,mode=0666", runMount{kind: "ssh", id: "gh", target: "/agent.sock", required: true, mode: 0o666}},
	} {
		t.Run(c.spec, func(t *testing.T) {
			if got := parseMount(c.spec, env, '\\', "/app"); !reflect.DeepEqual(got, c.want) {
				t.Fatalf("got %+v\nwant %+v", got, c.want)
			}
		})
	}
	for _, bad := range []string{"", "type=cache", "type=bind,target=/", "type=ssh,source=x", "type=ssh,env=A", "type=ssh,from=b", "type=nfs,target=/x", "type=bind,target=/x,id=a",
		"type=cache,target=/x,sharing=maybe", "type=bind,target=/x,sharing=locked", "type=bind,target=/x,mode=755", "type=cache,target=/x,mode=9",
		"type=bind,target=/x,required", "type=secret", "type=secret,id=a,source=b", "type=secret,id=a,from=b", "type=tmpfs,target=/x,from=a",
		"type=bind,target=/x,bogus=1", "type=bind,target=/x,ro=maybe", "type=bind,target"} {
		t.Run(bad, func(t *testing.T) { mustReject(t, func() { parseMount(bad, env, '\\', "/app") }) })
	}
}

func TestRunHeredocCommand(t *testing.T) {
	bd := &builder{escape: '\\'}
	st := &buildStage{config: obj{}}
	run := func(text string) ([]any, *runMount) {
		in, _ := parseDockerfile(text)
		return bd.runCommand(st, in[0])
	}
	// The document is the script, run by the shell (which also expands it).
	if cmd, script := run("RUN <<-EOF\n\techo $A\n\tEOF\n"); !reflect.DeepEqual(cmd, []any{"/bin/sh", "-c", "echo $A\n"}) || script != nil {
		t.Fatalf("shell script: %q %v", cmd, script)
	}
	// With #!, the interpreter runs the document, mounted at /dev/pipes/NAME.
	cmd, script := run("RUN <<'PY'\n#!/usr/bin/env python3 -u\nprint(1)\nPY\n")
	if !reflect.DeepEqual(cmd, []any{"/usr/bin/env", "python3 -u", "/dev/pipes/PY"}) || script == nil ||
		script.kind != "script" || script.target != "/dev/pipes/PY" || script.script != "#!/usr/bin/env python3 -u\nprint(1)\n" {
		t.Fatalf("#! script: %q %+v", cmd, script)
	}
	// Otherwise the shell gets the command line and the documents.
	st.config["Shell"] = []any{"/bin/bash", "-c"}
	if cmd, _ := run("RUN cat <<A > /a && python3 <<-'B'\na $X\nA\n\tprint(2)\n\tB\n"); !reflect.DeepEqual(cmd, []any{"/bin/bash", "-c", "cat <<A > /a && python3 <<-'B'\na $X\nA\n\tprint(2)\nB"}) {
		t.Fatalf("documents for the shell: %q", cmd)
	}
}

func TestExpandHeredoc(t *testing.T) {
	env := map[string]string{"A": "a"}
	got := expandHeredoc("$A ${A}b ${U:-d} 'q $A' \"$A\" \\$A \\\\ \\x $ $1 a\\\nb\n", env)
	if want := "a ab d 'q a' \"a\" $A \\ \\x $ $1 ab\n"; got != want {
		t.Fatalf("got %q, want %q", got, want)
	}
}

// TestMountStubs: mount points a step's container made are removed afterwards,
// unless the step wrote into them.
func TestMountStubs(t *testing.T) {
	root := t.TempDir()
	check(os.MkdirAll(filepath.Join(root, "run"), 0o755))
	check(os.Symlink("/run", filepath.Join(root, "var-run")))
	stubs := mountStubs(root, []string{"/var-run/secrets/a", "/run/secrets/b", "/mnt/src", "/run"})
	if want := []string{"run/secrets/a", "run/secrets/b", "run/secrets", "mnt/src", "mnt"}; !reflect.DeepEqual(stubs, want) {
		t.Fatalf("stubs %q, want %q", stubs, want)
	}
	for _, d := range []string{"run/secrets", "mnt/src"} {
		check(os.MkdirAll(filepath.Join(root, d), 0o755))
	}
	for _, f := range []string{"run/secrets/a", "run/secrets/b"} {
		check(os.WriteFile(filepath.Join(root, f), nil, 0o644))
	}
	check(os.WriteFile(filepath.Join(root, "mnt/kept"), []byte("x"), 0o644))
	removeStubs(root, stubs)
	for p, want := range map[string]bool{"run": true, "run/secrets": false, "mnt/src": false, "mnt/kept": true} {
		if exists(filepath.Join(root, p)) != want {
			t.Fatalf("%s exists: %v", p, !want)
		}
	}
}

// TestBuildMountsAndHeredocs runs RUN --mount and here-document steps in real
// containers through the classic API.
func TestBuildMountsAndHeredocs(t *testing.T) {
	b := testEngine(t)
	// No RUN: COPY here-documents and the errors a build reports before running.
	dockerfile := "# syntax=docker/dockerfile:1\nFROM dockiva/shell:demo\nARG NAME=x\nCOPY <<EOF <<'RAW' /docs/\nhi $NAME \"q\"\nEOF\nraw $NAME\nRAW\nCOPY --chmod=600 <<-EOF /one.txt\n\tone\n\tEOF\n"
	id, _, failure := buildResult(t, buildMessages(t, b, "t=heredocs", tarOf(t, map[string]string{"Dockerfile": dockerfile})))
	if failure != "" || id == "" {
		t.Fatalf("COPY heredocs: %q", failure)
	}
	root := b.image("heredocs").Root
	for name, want := range map[string]string{"docs/EOF": "hi x \"q\"\n", "docs/RAW": "raw $NAME\n", "one.txt": "one\n"} {
		if got, e := os.ReadFile(filepath.Join(root, name)); e != nil || string(got) != want {
			t.Fatalf("%s: %q %v", name, got, e)
		}
	}
	if st, _ := os.Stat(filepath.Join(root, "docs/EOF")); st.Mode().Perm() != 0o644 {
		t.Fatalf("heredoc file mode %v", st.Mode())
	}
	for dockerfile, want := range map[string]string{
		"FROM dockiva/shell:demo\nRUN --mount=type=secret,id=tok,required busybox true\n": "no such secret: the classic builder",
		"FROM dockiva/shell:demo\nRUN --mount=type=ssh,required busybox true\n":           "SSH agent forwarding needs buildx",
		"FROM dockiva/shell:demo\nRUN --security=insecure busybox true\n":                 "--security=insecure needs a privileged",
		"FROM dockiva/shell:demo\nRUN --network=bridge2 busybox true\n":                   "--network takes default, none or host",
		"FROM dockiva/shell:demo\nRUN --mount=type=bind,source=nope,target=/x true\n":     "nope not found in the build context",
		"# syntax=example.com/frontend:1\nFROM dockiva/shell:demo\n":                      "reads the docker/dockerfile syntax only",
	} {
		if _, _, failure := buildResult(t, buildMessages(t, b, "", tarOf(t, map[string]string{"Dockerfile": dockerfile}))); !strings.Contains(failure, want) {
			t.Fatalf("%q: error %q, want %q", dockerfile, failure, want)
		}
	}

	if !exists("../build/dockivad") {
		t.Skip("RUN needs ../build/dockivad (scripts/bundle_dockiva_engine.py)")
	}
	b.Self, b.Worker = "../build/dockivad", "../build/dockiva-worker"
	dockerfile = `FROM dockiva/shell:demo AS base
SHELL ["busybox", "sh", "-c"]
RUN busybox echo staged > /staged.txt
FROM base
WORKDIR /app
RUN --mount=type=cache,target=/var/cache/c,sharing=locked busybox sh -c 'busybox echo run >> /var/cache/c/log; busybox cat /var/cache/c/log > /app/runs'
RUN --mount=type=bind,source=src,target=/mnt/src --mount=type=bind,from=base,source=/staged.txt,target=s.txt busybox cat /mnt/src/a.txt /app/s.txt > /app/bound
RUN --mount=type=bind,source=src,target=/rw,rw --mount=type=tmpfs,target=/scratch busybox sh -c 'busybox echo w > /rw/a.txt; busybox echo t > /scratch/t; busybox ls /scratch > /app/tmp'
ARG V=v
RUN <<EOF
busybox echo "heredoc $V" > /app/heredoc
EOF
`
	build := func(query, a string) (string, string) {
		_, stream, failure := buildResult(t, buildMessages(t, b, query, tarOf(t, map[string]string{"Dockerfile": dockerfile, "src/a.txt": a})))
		return stream, failure
	}
	if stream, failure := build("t=mounts", "A\n"); failure != "" {
		t.Fatalf("mounts build: %q\n%s", failure, stream)
	}
	root = b.image("mounts").Root
	for name, want := range map[string]string{"app/runs": "run", "app/bound": "A\nstaged\n", "app/tmp": "t\n", "app/heredoc": "heredoc v\n"} {
		if got, e := os.ReadFile(filepath.Join(root, name)); e != nil || strings.TrimSpace(string(got)) != strings.TrimSpace(want) {
			t.Fatalf("%s: %q %v", name, got, e)
		}
	}
	for _, stub := range []string{"mnt/src", "app/s.txt", "rw", "scratch", "var/cache/c"} {
		if exists(filepath.Join(root, stub)) {
			t.Fatalf("mount point %s left in the image", stub)
		}
	}
	// A changed bind source reruns its step (and the rest); the cache mount keeps
	// its files across builds; the rw mount's write went nowhere.
	stream, failure := build("t=mounts", "B\n")
	if failure != "" || strings.Count(stream, " ---> Using cache\n") != 2 {
		t.Fatalf("changed bind source: %q\n%s", failure, stream)
	}
	if stream, failure = build("t=mounts&nocache=1", "B\n"); failure != "" {
		t.Fatalf("no-cache build: %q\n%s", failure, stream)
	}
	if got, _ := os.ReadFile(filepath.Join(b.image("mounts").Root, "app/runs")); string(got) != "run\nrun\n" {
		t.Fatalf("the cache mount was not kept: runs %q", got)
	}
	var mounts []obj
	for _, e := range b.buildCacheEntries() {
		if e.(obj)["Type"] == "exec.cachemount" {
			mounts = append(mounts, e.(obj))
		}
	}
	if len(mounts) != 1 || integer(mounts[0]["Size"]) != 8 || integer(mounts[0]["UsageCount"]) != 2 || len(buildCacheInUse) != 0 {
		t.Fatalf("cache mount entries: %v (in use %v)", mounts, buildCacheInUse)
	}
}
