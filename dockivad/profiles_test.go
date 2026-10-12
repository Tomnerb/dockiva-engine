package main

import (
	"bytes"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func cloneCatalog() profileCatalog { return loadProfileCatalog(profileCatalogJSON) }
func clonedConfig(c obj) obj       { return decode(profileBytes(c), maxJSON) }
func TestProfileSchemaAndSelection(t *testing.T) {
	catalog := cloneCatalog()
	pin := catalog.Images[0].ImageDigests[0]
	p := catalog.selectProfile(pin)
	if p.Definition.ID != "prepared-redis-7.4.2-native" || p.Definition.Backend != "hvf" || !p.Definition.Dynamic {
		t.Fatal("pinned backend lost")
	}
	if catalog.selectProfile("sha256:"+strings.Repeat("0", 64)).Definition.Backend != "interpreter" {
		t.Fatal("unknown image received acceleration")
	}
	cases := []func(*profileCatalog){
		func(c *profileCatalog) { c.SchemaVersion = 2 },
		func(c *profileCatalog) { c.Default.Backend = "hvf" },
		func(c *profileCatalog) { c.Images[0].Backend = "unicorn" }, /* removed backend */
		func(c *profileCatalog) { c.Images = append(c.Images, c.Images[0]) },
		func(c *profileCatalog) { other := c.Images[0]; other.ID = "other"; c.Images = append(c.Images, other) },
		func(c *profileCatalog) { c.Images[0].ImageDigests = []string{"not-a-digest"} },
		func(c *profileCatalog) { c.Images[0].MemoryLimitMiB = 1025 },
		func(c *profileCatalog) { c.Images[0].MaxSteps = 0 },
		func(c *profileCatalog) { c.Images[0].RequiredCapabilities = []string{"unknown-only"} },
		func(c *profileCatalog) { c.Images[0].Backend = "native-magic" },
		func(c *profileCatalog) { c.Images[0].WritableRoot = "anything" },
	}
	for i, mutate := range cases {
		t.Run(string(rune('A'+i)), func(t *testing.T) {
			c := cloneCatalog()
			mutate(&c)
			mustReject(t, func() { loadProfileCatalog(profileBytes(c)) })
		})
	}
	m := decode(profileCatalogJSON, 16384)
	m["ignored_option"] = true
	mustReject(t, func() { loadProfileCatalog(profileBytes(m)) })
	mustReject(t, func() { loadProfileCatalog([]byte(`{"schema_version":1,"schema_version":1}`)) })
}
func TestProfileNativeNegotiation(t *testing.T) {
	c := cloneCatalog()
	interpreter := probeRuntime(runtimeForBackend("../build/vmfree-c", "interpreter"))
	native := probeRuntime(runtimeForBackend(fakeRuntime(t, true), "hvf"))
	c.selectProfile("sha256:" + strings.Repeat("0", 64)).negotiate(interpreter)
	accelerated := c.selectProfile(c.Images[0].ImageDigests[0])
	accelerated.negotiate(native)
	mustReject(t, func() { accelerated.negotiate(interpreter) })
	accelerated.Definition.RequiredCapabilities = append(accelerated.Definition.RequiredCapabilities, "unimplemented-feature")
	mustReject(t, func() { accelerated.negotiate(native) })
	native.RuntimeABI = "future-abi"
	mustReject(t, func() { accelerated.negotiate(native) })
	mustReject(t, func() { runtimeForBackend("/missing/runtime", "hvf") })
	var output boundedProbeOutput
	_, err := io.Copy(&output, bytes.NewReader(make([]byte, 16385)))
	if err == nil || len(output.Bytes()) > 16384 {
		t.Fatal("probe output limit bypassed by io.Copy")
	}
	bad := filepath.Join(t.TempDir(), "bad-runtime")
	check(os.WriteFile(bad, []byte("#!/bin/sh\nprintf '%s' '{\"schema_version\":99,\"runtime_abi\":\"future\",\"backends\":{}}'\n"), 0700))
	mustReject(t, func() { probeRuntime(bad) })
}
func TestProfileAliasAndSavedPolicy(t *testing.T) {
	b := testEngine(t)
	b.Native = fakeRuntime(t, true) // the preset is pinned to the native backend
	commitImage(b.Cache, "alias/accelerated:renamed", "../examples/redis-preview/image")
	b.create("alias", decode([]byte(`{"Image":"alias/accelerated:renamed"}`), maxJSON))
	config := readObject(filepath.Join(b.State, "alias", "config.json"))
	p := savedProfile(config)
	if p.Definition.ID != "prepared-redis-7.4.2-native" {
		t.Fatal("image alias lost digest profile")
	}
	detail := object(b.inspect("alias")["DockivaCompatibility"])
	if detail["Backend"] != "hvf" || detail["Status"] != "EXPERIMENTAL" || detail["ImageDigest"] != config["image"] {
		t.Fatal(detail)
	}
	mustReject(t, func() {
		b.create("readonly", decode([]byte(`{"Image":"alias/accelerated:renamed","HostConfig":{"ReadonlyRootfs":true}}`), maxJSON))
	})
	if exists(filepath.Join(b.State, "readonly")) {
		t.Fatal("rejected profile published a container")
	}
	// The old reserved tag is not authority: content determines execution policy.
	commitImage(b.Cache, "vmfree/redis:preview", "../examples/m19/shell-image")
	b.create("renamed-shell", decode([]byte(`{"Image":"vmfree/redis:preview"}`), maxJSON))
	if object(b.inspect("renamed-shell")["DockivaCompatibility"])["ProfileID"] == "prepared-redis-7.4.2-native" {
		t.Fatal("tag name still selects the pinned profile")
	}
	for field, value := range map[string]any{"cpu_backend": "interpreter", "max_steps": 1, "image": "sha256:" + strings.Repeat("0", 64), "dynamic": false, "writable": false, "schedule": true, "host_sandbox": true} {
		t.Run(field, func(t *testing.T) {
			bad := clonedConfig(config)
			bad[field] = value
			mustReject(t, func() { savedProfile(bad) })
		})
	}
	bad := clonedConfig(config)
	object(bad["runtime_profile"])["schema_version"] = 2
	mustReject(t, func() { savedProfile(bad) })
	bad = clonedConfig(config)
	object(object(bad["runtime_profile"])["definition"])["scope"] = "changed"
	mustReject(t, func() { savedProfile(bad) })
	old := clonedConfig(config)
	delete(old, "runtime_profile")
	if savedProfile(old) != nil || profileInspection(old)["Source"] != "legacy-state" {
		t.Fatal("legacy configuration silently migrated")
	}
	// A deployment catalog update cannot rewrite a previously persisted policy.
	updated := cloneCatalog()
	updated.Images[0].MaxSteps = 99
	updated.Images[0].Revision++
	if savedProfile(config).Definition.MaxSteps != cloneCatalog().Images[0].MaxSteps || updated.selectProfile(p.ImageDigest).Definition.MaxSteps != 99 {
		t.Fatal("saved policy changed with catalog")
	}
	// A container saved under a since-removed backend stays listable; only starting it fails.
	removed := clonedConfig(config)
	object(object(removed["runtime_profile"])["definition"])["backend"] = "unicorn"
	removed["cpu_backend"] = "unicorn"
	save(filepath.Join(b.State, "alias", "config.json"), removed)
	if detail := object(b.inspect("alias")["DockivaCompatibility"]); detail["Status"] != "UNSUPPORTED" || detail["Backend"] != "unicorn" {
		t.Fatal(detail)
	}
	if response := request(t, b.handler("secret"), "GET", "/containers/json?all=1", "", "secret"); response.Code != 200 {
		t.Fatalf("listing failed: %d %s", response.Code, response.Body.String())
	}
	mustReject(t, func() { validateLaunchProfile(removed, "") })
}
func TestProfilePreflightRejectsBeforePublicationOrStart(t *testing.T) {
	b := testEngine(t)
	original := append([]byte(nil), profileCatalogJSON...)
	defer func() { profileCatalogJSON = original }()
	c := cloneCatalog()
	c.Default.RequiredCapabilities = append(c.Default.RequiredCapabilities, "unimplemented-feature")
	profileCatalogJSON = profileBytes(c)
	response := request(t, b.handler("secret"), "POST", "/containers/create?name=rejected", `{"Image":"vmfree/shell:demo"}`, "secret")
	if response.Code != 409 || !strings.Contains(response.Body.String(), "unimplemented-feature") {
		t.Fatalf("%d %s", response.Code, response.Body.String())
	}
	if exists(filepath.Join(b.State, "rejected")) {
		t.Fatal("unsupported requirements created state")
	}
	profileCatalogJSON = original
	b.create("valid", decode([]byte(`{"Image":"vmfree/shell:demo"}`), maxJSON))
	p := filepath.Join(b.State, "valid")
	config := readObject(filepath.Join(p, "config.json"))
	config["max_steps"] = json.Number("42")
	save(filepath.Join(p, "config.json"), config)
	mustReject(t, func() { b.start("valid") })
	if readObject(filepath.Join(p, "status.json"))["state"] != "created" || exists(filepath.Join(p, "control.json")) {
		t.Fatal("invalid saved policy started a worker")
	}
}

func TestDefaultProfileRunsDynamicUnbounded(t *testing.T) {
	p := loadProfileCatalog(profileCatalogJSON).selectProfile("sha256:" + strings.Repeat("1", 64))
	if p.Definition.Backend != "interpreter" || !p.Definition.Dynamic || !p.Definition.ProcessTree || !p.Definition.ScheduledNetwork || p.Definition.MaxSteps != ^uint64(0) {
		t.Fatalf("default profile: %+v", p.Definition)
	}
	config := obj{"image": p.ImageDigest, "writable": true}
	p.apply(config)
	for k, v := range map[string]any{"schedule": false, "job_control": false, "host_sandbox": false} {
		config[k] = v
	}
	// The saved policy must survive a JSON round trip at UINT64_MAX.
	if savedProfile(decode(profileBytes(config), maxJSON)) == nil {
		t.Fatal("saved default profile rejected")
	}
}

// fakeRuntime writes vmfree-c and vmfree-c-hvf scripts that answer the capability
// probe; hvf controls whether the hypervisor backend is advertised.
func fakeRuntime(t *testing.T, hvf bool) string {
	dir := t.TempDir()
	backends := `"interpreter":["elf64-arm64","private-rootfs","loopback-tcp","bounded-dynamic-elf"]`
	if hvf {
		backends += `,"hvf":["elf64-arm64","private-rootfs","loopback-tcp","bounded-dynamic-elf","native-threads"]`
	}
	script := "#!/bin/sh\necho '{\"schema_version\":1,\"runtime_abi\":\"" + runtimeABI + "\",\"backends\":{" + backends + "}}'\n"
	for _, name := range []string{"vmfree-c", "vmfree-c-hvf"} {
		check(os.WriteFile(filepath.Join(dir, name), []byte(script), 0700))
	}
	return filepath.Join(dir, "vmfree-c")
}
func TestNativeProfileSelection(t *testing.T) {
	for _, hvf := range []bool{true, false} {
		b := testEngine(t)
		b.Native = fakeRuntime(t, hvf)
		b.create("c", decode([]byte(`{"Image":"vmfree/shell:demo"}`), maxJSON))
		config := readObject(filepath.Join(b.State, "c", "config.json"))
		want := map[bool]string{true: "hvf", false: "interpreter"}[hvf]
		if config["cpu_backend"] != want || savedProfile(config).Definition.Backend != want {
			t.Fatalf("hvf=%v: backend %v", hvf, config["cpu_backend"])
		}
		if hvf && (config["process_tree"] != true || config["scheduled_network"] != true || config["dynamic"] != true) {
			t.Fatalf("native policy incomplete: %v", config)
		}
	}
	// A pinned image keeps its own profile even where native execution is available.
	b := testEngine(t)
	b.Native = fakeRuntime(t, true)
	pin := cloneCatalog().Images[0].ImageDigests[0]
	if p := cloneCatalog().selectForHost(pin, b.Native); p.Definition.ID != "prepared-redis-7.4.2-native" {
		t.Fatalf("pinned image lost its profile: %v", p.Definition.ID)
	}
	c := cloneCatalog()
	c.Native.Backend = "interpreter"
	mustReject(t, func() { loadProfileCatalog(profileBytes(c)) })
	c = cloneCatalog()
	c.Native.ProcessTree = false
	mustReject(t, func() { loadProfileCatalog(profileBytes(c)) })
}
