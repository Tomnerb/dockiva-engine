package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestBindsMountsAndVolumes(t *testing.T) {
	b := testEngine(t)
	h := b.handler("secret")
	src := t.TempDir()
	missing := filepath.Join(t.TempDir(), "created-by-v")
	body := `{"Image":"dockiva/shell:demo","Volumes":{"/anon":{}},"HostConfig":{"Binds":["` + src + `:/app:ro","` + missing + `:/new","data:/var/data"],` +
		`"Mounts":[{"Type":"volume","Target":"/cache"},{"Type":"bind","Source":"` + src + `","Target":"/again"}]}}`
	if r := request(t, h, "POST", "/containers/create?name=mounts", body, "secret"); r.Code != 201 {
		t.Fatalf("create: %d %s", r.Code, r.Body.String())
	}
	if st, e := os.Stat(missing); e != nil || !st.IsDir() {
		t.Fatal("-v did not create the missing host directory")
	}
	var detail struct {
		Mounts     []map[string]any
		HostConfig struct{ Binds []string }
	}
	check(json.Unmarshal(request(t, h, "GET", "/containers/mounts/json", "", "secret").Body.Bytes(), &detail))
	if len(detail.Mounts) != 6 || len(detail.HostConfig.Binds) != 3 {
		t.Fatalf("mounts: %+v", detail)
	}
	kinds := map[string]string{}
	for _, m := range detail.Mounts {
		kinds[m["Destination"].(string)] = m["Type"].(string) + map[bool]string{true: " rw", false: " ro"}[m["RW"].(bool)]
	}
	if kinds["/app"] != "bind ro" || kinds["/var/data"] != "volume rw" || kinds["/cache"] != "volume rw" || kinds["/anon"] != "volume rw" {
		t.Fatalf("mount kinds: %v", kinds)
	}
	// The named volume exists and is in use; the anonymous ones are listed too.
	if r := request(t, h, "GET", "/volumes/data", "", "secret"); r.Code != 200 || !strings.Contains(r.Body.String(), `"Driver":"local"`) {
		t.Fatalf("volume inspect: %d %s", r.Code, r.Body.String())
	}
	if r := request(t, h, "DELETE", "/volumes/data", "", "secret"); r.Code != 409 {
		t.Fatalf("removing a volume in use: %d", r.Code)
	}
	var list struct{ Volumes []any }
	check(json.Unmarshal(request(t, h, "GET", "/volumes", "", "secret").Body.Bytes(), &list))
	if len(list.Volumes) != 3 {
		t.Fatalf("volumes: %d", len(list.Volumes))
	}
	// docker rm -v removes the anonymous volumes; the named one stays and is now removable.
	if request(t, h, "DELETE", "/containers/mounts?v=1", "", "secret").Code != 204 {
		t.Fatal("remove container")
	}
	check(json.Unmarshal(request(t, h, "GET", "/volumes", "", "secret").Body.Bytes(), &list))
	if len(list.Volumes) != 1 || request(t, h, "DELETE", "/volumes/data", "", "secret").Code != 204 {
		t.Fatalf("after rm -v: %d volumes", len(list.Volumes))
	}
	for _, bad := range []string{`{"Image":"dockiva/shell:demo","HostConfig":{"Binds":["relative:/x:bad"]}}`, `{"Image":"dockiva/shell:demo","HostConfig":{"Binds":["` + src + `:relative"]}}`,
		`{"Image":"dockiva/shell:demo","HostConfig":{"Binds":["` + src + `:/"]}}`, `{"Image":"dockiva/shell:demo","HostConfig":{"Binds":["` + src + `:/x","` + src + `:/x"]}}`,
		`{"Image":"dockiva/shell:demo","HostConfig":{"Mounts":[{"Type":"bind","Source":"/no/such/dir","Target":"/x"}]}}`,
		`{"Image":"dockiva/shell:demo","HostConfig":{"Mounts":[{"Type":"tmpfs","Source":"x","Target":"/x"}]}}`,
		`{"Image":"dockiva/shell:demo","HostConfig":{"Tmpfs":{"/x":"size=lots"}}}`, `{"Image":"dockiva/shell:demo","HostConfig":{"Tmpfs":{"/x":"bogus"}}}`,
		`{"Image":"dockiva/shell:demo","HostConfig":{"Tmpfs":{"/x":""},"Binds":["` + src + `:/x"]}}`} {
		if r := request(t, h, "POST", "/containers/create?name=bad", bad, "secret"); r.Code < 400 || (r.Code >= 500 && r.Code != 501) {
			t.Fatalf("%s: %d %s", bad, r.Code, r.Body.String())
		}
	}
	// tmpfs: --tmpfs in HostConfig.Tmpfs, --mount type=tmpfs in Mounts; Docker's default options first.
	tmpfs := `{"Image":"dockiva/shell:demo","HostConfig":{"Tmpfs":{"/run":"size=1m,exec"},"Mounts":[{"Type":"tmpfs","Target":"/scratch","TmpfsOptions":{"SizeBytes":1048576,"Mode":448}}]}}`
	if r := request(t, h, "POST", "/containers/create?name=tm", tmpfs, "secret"); r.Code != 201 {
		t.Fatalf("tmpfs create: %d %s", r.Code, r.Body.String())
	}
	r := request(t, h, "GET", "/containers/tm/json", "", "secret")
	if !strings.Contains(r.Body.String(), `"Tmpfs":{"/run":"size=1m,exec"}`) || !strings.Contains(r.Body.String(), `{"Destination":"/scratch","Mode":"","Propagation":"","RW":true,"Source":"","Type":"tmpfs"}`) {
		t.Fatalf("tmpfs inspect: %s", r.Body.String())
	}
	saved := savedMounts(readObject(filepath.Join(b.State, "tm", "config.json")))
	if len(saved) != 2 || saved[0]["options"] != "noexec,nosuid,nodev,size=1m,exec" || saved[1]["options"] != "noexec,nosuid,nodev,size=1048576,mode=700" {
		t.Fatalf("tmpfs saved: %v", saved)
	}
	request(t, h, "DELETE", "/containers/tm", "", "secret")
	// Volumes created through the API, and prune of unused anonymous ones.
	if r := request(t, h, "POST", "/volumes/create", `{"Name":"made","Labels":{"k":"v"}}`, "secret"); r.Code != 201 {
		t.Fatalf("volume create: %d %s", r.Code, r.Body.String())
	}
	request(t, h, "POST", "/volumes/create", `{}`, "secret")
	r = request(t, h, "POST", "/volumes/prune", "", "secret")
	if r.Code != 200 || strings.Contains(r.Body.String(), `"made"`) || !strings.Contains(r.Body.String(), `"VolumesDeleted":["`) {
		t.Fatalf("prune: %d %s", r.Code, r.Body.String())
	}
}

func TestMountPointsAndPopulation(t *testing.T) {
	home := t.TempDir()
	check(os.Chmod(home, 0700))
	root := filepath.Join(home, "rootfs")
	check(os.MkdirAll(filepath.Join(root, "real", "seed"), 0o755))
	check(os.WriteFile(filepath.Join(root, "real", "seed", "file"), []byte("from image"), 0o644))
	check(os.Symlink("/real", filepath.Join(root, "link")))
	b := &engine{Home: home}
	b.createVolume("vol", obj{}, false)
	file := filepath.Join(t.TempDir(), "conf")
	check(os.WriteFile(file, []byte("x"), 0o644))
	mounts := obj{"mounts": []any{
		obj{"type": "volume", "source": "vol", "target": "/link/seed", "readonly": false},
		obj{"type": "bind", "source": file, "target": "/etc/app/conf", "readonly": true},
		obj{"type": "tmpfs", "target": "/link/scratch", "options": "noexec,nosuid,nodev,size=1m", "readonly": false},
	}}
	args := prepareMounts(home, layers{upper: root}, mounts, true)
	// The mount point is resolved through the image's symlink; the volume got the image's content.
	if strings.Join(args, " ") != "--bind "+volumeData(home, "vol")+" /real/seed --bind-ro "+file+" /etc/app/conf --tmpfs /real/scratch:noexec,nosuid,nodev,size=1m" {
		t.Fatalf("args: %v", args)
	}
	if args := prepareMounts(home, layers{upper: root}, mounts, false); len(args) != 6 { // an exec run: no tmpfs
		t.Fatalf("exec args: %v", args)
	}
	if st, e := os.Stat(filepath.Join(root, "real", "scratch")); e != nil || !st.IsDir() {
		t.Fatal("tmpfs mount point not made")
	}
	if b, e := os.ReadFile(filepath.Join(volumeData(home, "vol"), "file")); e != nil || string(b) != "from image" {
		t.Fatal("volume not populated from the image")
	}
	if st, e := os.Lstat(filepath.Join(root, "etc", "app", "conf")); e != nil || !st.Mode().IsRegular() {
		t.Fatal("file mount point not created")
	}
}
