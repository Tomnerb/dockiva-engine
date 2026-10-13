package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestRegisterDockerContext(t *testing.T) {
	config := t.TempDir()
	t.Setenv("DOCKER_CONFIG", config)
	registerDockerContext("/tmp/vmfree-test/docker.sock")
	meta := filepath.Join(config, "contexts", "meta", key(dockerContextName), "meta.json")
	raw, e := os.ReadFile(meta)
	if e != nil || !strings.Contains(string(raw), `"Host":"unix:///tmp/vmfree-test/docker.sock"`) {
		t.Fatalf("context not written: %v %s", e, raw)
	}
	// The docker CLI, when installed, lists it without it becoming the current context.
	if docker, e := exec.LookPath("docker"); e == nil {
		out, e := exec.Command(docker, "--config", config, "context", "ls", "--format", "{{.Name}} {{.Current}} {{.DockerEndpoint}}").CombinedOutput()
		if e != nil || !strings.Contains(string(out), "dockiva false unix:///tmp/vmfree-test/docker.sock") {
			t.Fatalf("docker context ls: %v %s", e, out)
		}
	}
	// A context of that name that Dockiva did not create is left alone.
	check(os.WriteFile(meta, []byte(`{"Name":"dockiva","Metadata":{"Description":"mine"},"Endpoints":{"docker":{"Host":"unix:///elsewhere"}}}`), 0o644))
	registerDockerContext("/tmp/vmfree-test/docker.sock")
	if raw, _ := os.ReadFile(meta); !strings.Contains(string(raw), "elsewhere") {
		t.Fatal("user's own context replaced")
	}
}
