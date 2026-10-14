package main

import (
	"bytes"
	"context"
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sync"
	"time"
)

const runtimeABI = "dockiva-native-v1"

// The catalog is trusted deployment policy, never read from guest image labels.
//
//go:embed profiles.json
var profileCatalogJSON []byte

type executionProfile struct {
	ID                   string   `json:"id"`
	Revision             int      `json:"revision"`
	ImageDigests         []string `json:"image_digests"`
	Backend              string   `json:"backend"`
	RequiredCapabilities []string `json:"required_capabilities"`
	Dynamic              bool     `json:"dynamic"`
	// omitempty keeps definition digests of profiles saved before this field unchanged.
	ProcessTree      bool   `json:"process_tree,omitempty"`
	ScheduledNetwork bool   `json:"scheduled_network,omitempty"`
	WritableRoot     string `json:"writable_root"`
	MemoryLimitMiB   int64  `json:"memory_limit_mib"`
	MaxSteps         uint64 `json:"max_steps"`
	Scope            string `json:"scope"`
}
type profileCatalog struct {
	SchemaVersion int              `json:"schema_version"`
	Default       executionProfile `json:"default"`
	// Native, when present, replaces Default for unpinned images wherever the
	// hypervisor runtime probes as usable; otherwise Default applies.
	Native *executionProfile  `json:"native,omitempty"`
	Images []executionProfile `json:"images"`
}
type effectiveProfile struct {
	SchemaVersion    int              `json:"schema_version"`
	RuntimeABI       string           `json:"runtime_abi"`
	ImageDigest      string           `json:"image_digest"`
	DefinitionDigest string           `json:"definition_digest"`
	Definition       executionProfile `json:"definition"`
}
type runtimeCapabilities struct {
	SchemaVersion int                 `json:"schema_version"`
	RuntimeABI    string              `json:"runtime_abi"`
	Backends      map[string][]string `json:"backends"`
}

var profileID = regexp.MustCompile(`^[a-z0-9][a-z0-9.-]{0,79}$`)

func strictProfileJSON(raw []byte, out any) {
	decode(raw, 16384) // Includes duplicate-key and UTF-8 checks.
	d := json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	reject(d.Decode(out) == nil, "Invalid compatibility profile schema")
}
func profileBytes(value any) []byte {
	raw, err := json.Marshal(value)
	check(err)
	return raw
}
func validateProfile(p executionProfile) {
	reject(profileID.MatchString(p.ID) && p.Revision > 0, "Invalid profile ID/revision")
	reject(p.Backend == "interpreter" || p.Backend == "hvf", "Unknown profile backend")
	reject(p.WritableRoot == "optional" || p.WritableRoot == "required", "Invalid profile writable-root policy")
	reject(p.MemoryLimitMiB >= 1 && p.MemoryLimitMiB <= 65536, "Invalid profile memory limit")
	reject(p.MaxSteps > 0, "Profile must bound guest instruction execution")
	reject(len(p.Scope) > 0 && len(p.Scope) <= 1024, "Profile requires bounded evidence scope")
	reject(len(p.ImageDigests) <= 32, "Too many profile image digests")
	seen := map[string]bool{}
	for _, d := range p.ImageDigests {
		reject(digestPattern.MatchString(d) && !seen[d], "Invalid/duplicate profile digest")
		seen[d] = true
	}
	reject(len(p.RequiredCapabilities) > 0 && len(p.RequiredCapabilities) <= 32, "Invalid required capabilities")
	seen = map[string]bool{}
	for _, capability := range p.RequiredCapabilities {
		reject(profileID.MatchString(capability) && !seen[capability], "Invalid/duplicate capability")
		seen[capability] = true
	}
	reject(seen["elf64-arm64"] && seen["private-rootfs"], "Profile must require ARM64 ELF and private rootfs")
	reject(!p.Dynamic || seen["bounded-dynamic-elf"], "Dynamic profile must require loader capability")
	// Native execution maps whole pages and blocks through the scheduler.
	reject(p.Backend != "hvf" || (p.Dynamic && p.ProcessTree && p.ScheduledNetwork), "Native profile requires dynamic mode, process trees and scheduled networking")
	// Inside a process tree the scheduler runs, and sockets then need scheduler-aware networking.
	reject(!p.ProcessTree || p.Backend != "interpreter" || p.ScheduledNetwork, "Interpreter process trees require scheduled networking")
}
func loadProfileCatalog(raw []byte) profileCatalog {
	var c profileCatalog
	strictProfileJSON(raw, &c)
	reject(c.SchemaVersion == 1, "Unsupported profile catalog version")
	validateProfile(c.Default)
	reject(c.Default.Backend == "interpreter" && len(c.Default.ImageDigests) == 0, "Default profile must use the unbound interpreter policy")
	if c.Native != nil {
		validateProfile(*c.Native)
		reject(c.Native.Backend == "hvf" && len(c.Native.ImageDigests) == 0 && c.Native.ID != c.Default.ID, "Native profile must be an unbound hvf policy")
	}
	reject(len(c.Images) <= 32, "Too many image profiles")
	ids := map[string]bool{c.Default.ID: true}
	digests := map[string]bool{}
	for _, p := range c.Images {
		validateProfile(p)
		reject(!ids[p.ID] && len(p.ImageDigests) > 0, "Duplicate/unbound image profile")
		ids[p.ID] = true
		for _, d := range p.ImageDigests {
			reject(!digests[d], "Ambiguous image profile digest")
			digests[d] = true
		}
	}
	return c
}

// selectForHost picks the image's pinned profile, else the native profile when this
// host can run it, else the default. The probe result is cached per engine process.
func (c profileCatalog) selectForHost(imageDigest, native string) effectiveProfile {
	p := c.selectProfile(imageDigest)
	if c.Native == nil || p.Definition.ID != c.Default.ID || !nativeUsable(native) {
		return p
	}
	return effectiveProfile{1, runtimeABI, imageDigest, digest(profileBytes(*c.Native)), *c.Native}
}

var nativeProbe = struct {
	sync.Mutex
	known map[string]bool // by interpreter path
}{known: map[string]bool{}}

func nativeUsable(native string) bool {
	nativeProbe.Lock()
	defer nativeProbe.Unlock()
	if usable, ok := nativeProbe.known[native]; ok {
		return usable
	}
	usable := safeRun(func() {
		capabilities := probeRuntime(runtimeForBackend(native, "hvf"))
		_, ok := capabilities.Backends["hvf"]
		reject(ok, "hvf unavailable")
	}) == nil
	nativeProbe.known[native] = usable
	return usable
}
func (c profileCatalog) selectProfile(imageDigest string) effectiveProfile {
	reject(digestPattern.MatchString(imageDigest), "Invalid profile image digest")
	p := c.Default
	for _, candidate := range c.Images {
		for _, d := range candidate.ImageDigests {
			if d == imageDigest {
				p = candidate
			}
		}
	}
	return effectiveProfile{1, runtimeABI, imageDigest, digest(profileBytes(p)), p}
}
func (p effectiveProfile) apply(config obj) {
	config["runtime_profile"] = p
	config["cpu_backend"] = p.Definition.Backend
	config["dynamic"] = p.Definition.Dynamic
	config["process_tree"] = p.Definition.ProcessTree
	config["scheduled_network"] = p.Definition.ScheduledNetwork
	config["memory_limit_mib"] = p.Definition.MemoryLimitMiB
	config["max_steps"] = p.Definition.MaxSteps
	reject(p.Definition.WritableRoot != "required" || config["writable"] == true, "Compatibility profile requires a private writable root")
}
func savedProfile(config obj) *effectiveProfile {
	raw, exists := config["runtime_profile"]
	if !exists {
		return nil
	} // Legacy policy remains explicit and unchanged.
	var p effectiveProfile
	strictProfileJSON(profileBytes(raw), &p)
	reject(p.SchemaVersion == 1 && p.RuntimeABI == runtimeABI, "Unsupported saved profile contract")
	validateProfile(p.Definition)
	reject(p.ImageDigest == config["image"] && digestPattern.MatchString(p.ImageDigest), "Saved profile image digest mismatch")
	reject(p.DefinitionDigest == digest(profileBytes(p.Definition)), "Saved profile definition changed")
	if len(p.Definition.ImageDigests) > 0 {
		match := false
		for _, d := range p.Definition.ImageDigests {
			match = match || d == p.ImageDigest
		}
		reject(match, "Profile is not bound to this image")
	}
	expected := obj{"cpu_backend": p.Definition.Backend, "dynamic": p.Definition.Dynamic,
		"memory_limit_mib": p.Definition.MemoryLimitMiB, "max_steps": p.Definition.MaxSteps,
		"schedule": false, "process_tree": p.Definition.ProcessTree, "scheduled_network": p.Definition.ScheduledNetwork, "job_control": false, "host_sandbox": false}
	for field, value := range expected {
		reject(bytes.Equal(profileBytes(config[field]), profileBytes(value)), "Saved runtime policy disagrees with profile: "+field)
	}
	reject(p.Definition.WritableRoot != "required" || config["writable"] == true, "Saved profile requires a private writable root")
	return &p
}
func runtimeForBackend(native, backend string) string {
	if backend == "hvf" {
		native = engineBinary(filepath.Dir(native), "dockiva-hvf", "vmfree-c-hvf")
	}
	st, err := os.Stat(native)
	if err != nil || !st.Mode().IsRegular() || st.Mode().Perm()&0111 == 0 {
		panic(apiError{409, "Required runtime backend unavailable: " + backend})
	}
	return regularExecutable(native)
}

type boundedProbeOutput struct{ data bytes.Buffer }

func (b *boundedProbeOutput) Bytes() []byte { return b.data.Bytes() }

func (b *boundedProbeOutput) Write(p []byte) (int, error) {
	if b.data.Len()+len(p) > 16384 {
		return 0, fmt.Errorf("runtime capability response exceeds limit")
	}
	return b.data.Write(p)
}
func probeRuntime(native string) runtimeCapabilities {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, native, "capabilities")
	var stdout, stderr boundedProbeOutput
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	cmd.Env = []string{"PATH=/usr/bin:/bin", "LANG=C"}
	if err := cmd.Run(); err != nil {
		panic(apiError{409, "Runtime capability probe failed: " + err.Error()})
	}
	var capabilities runtimeCapabilities
	strictProfileJSON(stdout.Bytes(), &capabilities)
	reject(capabilities.SchemaVersion == 1 && capabilities.RuntimeABI == runtimeABI, "Unsupported native runtime capability contract")
	return capabilities
}
func (p effectiveProfile) negotiate(capabilities runtimeCapabilities) {
	if capabilities.SchemaVersion != 1 || capabilities.RuntimeABI != p.RuntimeABI {
		panic(apiError{409, "Runtime ABI does not satisfy compatibility profile"})
	}
	available, ok := capabilities.Backends[p.Definition.Backend]
	if !ok {
		panic(apiError{409, "Runtime lacks required backend: " + p.Definition.Backend})
	}
	for _, required := range p.Definition.RequiredCapabilities {
		found := false
		for _, feature := range available {
			found = found || feature == required
		}
		if !found {
			panic(apiError{409, "Runtime lacks required capability: " + required})
		}
	}
}

// savedProfileProblem says why a container's saved profile cannot be used (for
// example its backend was removed from this engine), or "" when it can.
func savedProfileProblem(config obj) (problem string) {
	defer func() {
		if r := recover(); r != nil {
			e, ok := r.(apiError)
			if !ok {
				panic(r)
			}
			problem = "Saved compatibility profile is unusable (" + e.Message + "); recreate the container"
		}
	}()
	savedProfile(config)
	return ""
}
func validateLaunchProfile(config obj, native string) {
	problem := savedProfileProblem(config)
	reject(problem == "", problem)
	if p := savedProfile(config); p != nil {
		p.negotiate(probeRuntime(runtimeForBackend(native, p.Definition.Backend)))
	}
}
func profileInspection(config obj) obj {
	// One unusable container must not break listing the others; starting it fails.
	if problem := savedProfileProblem(config); problem != "" {
		return obj{"Status": "UNSUPPORTED", "Source": "digest-profile", "Backend": text(config["cpu_backend"]),
			"Reason": problem, "RuntimeABI": nil, "ImageDigest": config["image"]}
	}
	p := savedProfile(config)
	if p == nil {
		return obj{"Status": "EXPERIMENTAL", "Source": "legacy-state", "Backend": text(config["cpu_backend"]),
			"Reason": "Legacy launch configuration has no negotiated compatibility profile", "RuntimeABI": nil, "ImageDigest": config["image"]}
	}
	return obj{"Status": "EXPERIMENTAL", "Source": "digest-profile", "ProfileID": p.Definition.ID,
		"Revision": p.Definition.Revision, "SchemaVersion": p.SchemaVersion, "ImageDigest": p.ImageDigest,
		"DefinitionDigest": p.DefinitionDigest, "RuntimeABI": p.RuntimeABI, "Backend": p.Definition.Backend,
		"RequiredCapabilities": p.Definition.RequiredCapabilities, "Policy": p.Definition,
		"Reason": p.Definition.Scope}
}
