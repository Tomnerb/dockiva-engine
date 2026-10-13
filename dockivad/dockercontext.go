package main

import (
	"encoding/json"
	"os"
	"path/filepath"
)

const dockerContextName = "dockiva"
const dockerContextDescription = "Dockiva VM-free engine"

// registerDockerContext adds (or refreshes) the "dockiva" Docker context for the
// user-only engine socket, so `docker context use dockiva` points the docker CLI at
// this engine. It never changes the current context, and a context of that name
// that Dockiva did not create is left alone.
func registerDockerContext(socket string) {
	config := os.Getenv("DOCKER_CONFIG")
	if config == "" {
		home, e := os.UserHomeDir()
		check(e)
		config = filepath.Join(home, ".docker")
	}
	// The Docker CLI stores a context under the SHA-256 of its name.
	dir := filepath.Join(config, "contexts", "meta", key(dockerContextName))
	path := filepath.Join(dir, "meta.json")
	if raw, e := os.ReadFile(path); e == nil {
		var existing struct{ Metadata struct{ Description string } }
		if json.Unmarshal(raw, &existing) != nil || existing.Metadata.Description != dockerContextDescription {
			return
		}
	}
	meta, e := json.Marshal(obj{"Name": dockerContextName, "Metadata": obj{"Description": dockerContextDescription},
		"Endpoints": obj{"docker": obj{"Host": "unix://" + socket, "SkipTLSVerify": false}}})
	check(e)
	check(os.MkdirAll(dir, 0o755))
	temp := path + ".tmp"
	check(os.WriteFile(temp, meta, 0o644))
	check(os.Rename(temp, path))
}
