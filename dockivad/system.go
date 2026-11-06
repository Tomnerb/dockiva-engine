package main

// docker system df, the prune endpoints, and POST /dockiva/shutdown.

import (
	"fmt"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"
)

// systemAPI serves /system/df, /containers/prune, /images/prune, /build/prune and
// /dockiva/shutdown.
func (b *engine) systemAPI(w http.ResponseWriter, method, path string, q url.Values) bool {
	switch {
	case path == "/system/df" && method == "GET":
		send(w, 200, b.diskUsage())
	case path == "/containers/prune" && method == "POST":
		_, keep := pruneFilter(q)
		b.Mu.Lock()
		defer b.Mu.Unlock()
		deleted, reclaimed := []string{}, int64(0)
		for _, name := range b.names() {
			_ = safeRun(func() {
				item := b.inspect(name)
				created, e := time.Parse(time.RFC3339Nano, str(item["Created"]))
				check(e)
				if object(item["State"])["Running"] == true || !keep(created, object(object(item["Config"])["Labels"])) {
					return
				}
				size := diskSize(filepath.Join(b.container(name), "rootfs"))
				b.remove(name)
				deleted, reclaimed = append(deleted, str(item["Id"])), reclaimed+max(size, 0)
			})
		}
		send(w, 200, obj{"ContainersDeleted": deleted, "SpaceReclaimed": reclaimed})
	case path == "/images/prune" && method == "POST":
		filters, keep := pruneFilter(q, "dangling")
		danglingOnly := true // Docker's default: untagged images only
		for _, v := range filterValues(filters["dangling"]) {
			reject(v == "true" || v == "1" || v == "false" || v == "0", "Invalid dangling filter: "+v)
			danglingOnly = v == "true" || v == "1"
		}
		b.Mu.Lock()
		defer b.Mu.Unlock()
		send(w, 200, b.pruneImages(danglingOnly, keep))
	case path == "/build/prune" && method == "POST":
		// ponytail: until is the only filter (on last use); all=1 changes nothing, as no entry is ever in use.
		_, keep := pruneFilter(q)
		limit := int64(0)
		if v := q.Get("keep-storage"); v != "" {
			n, e := strconv.ParseInt(v, 10, 64)
			reject(e == nil && n >= 0, "Invalid keep-storage: "+v)
			limit = n
		}
		buildCacheMu.Lock()
		defer buildCacheMu.Unlock()
		deleted, reclaimed := b.pruneBuildCache(limit, func(used time.Time) bool { return keep(used, nil) })
		send(w, 200, obj{"CachesDeleted": deleted, "SpaceReclaimed": reclaimed})
	case path == "/dockiva/shutdown" && method == "POST":
		unbounded(w) // stop grace periods
		b.Mu.Lock()  // held until the engine exits: nothing starts meanwhile
		b.stopAll()
		send(w, 204, nil)
		if f, ok := w.(http.Flusher); ok {
			f.Flush()
		}
		select { // main shuts the API down as on SIGTERM
		case b.quit <- struct{}{}:
		default:
		}
	default:
		return false
	}
	return true
}

// diskSize is a tree's file bytes (treeSize: symlinks are not followed), -1 when it
// cannot be read (removed meanwhile, say).
func diskSize(root string) int64 {
	n := int64(-1)
	_ = safeRun(func() { n = treeSize(root) })
	return n
}

// diskUsage is GET /system/df. Images use their recorded size; each container root
// and volume is walked once. A layered container's SizeRw is its own files, and
// SizeRootFs adds its image's (ponytail: a --rootfs copy one's are its whole clone).
func (b *engine) diskUsage() obj {
	users, mounts := map[string]int64{}, map[string]int64{} // containers per image digest, per volume
	containers, layered := []any{}, map[string][]obj{}      // layered rows by image digest
	for _, name := range b.names() {
		_ = safeRun(func() {
			item := b.inspect(name)
			row := psRow(item)
			size := diskSize(filepath.Join(b.container(name), "rootfs"))
			row["SizeRw"], row["SizeRootFs"] = size, size
			if item["Driver"] == "overlay2" {
				layered[str(item["Image"])] = append(layered[str(item["Image"])], row)
			}
			users[str(item["Image"])]++
			for _, m := range item["Mounts"].([]any) {
				if v, ok := m.(obj)["Name"].(string); ok {
					mounts[v]++
				}
			}
			containers = append(containers, row)
		})
	}
	// One row per image, with its tags (they share its files), as Docker lists them.
	images, rows, layers := []any{}, map[string]obj{}, int64(0)
	for _, c := range b.cachedImages() {
		row := rows[c.digest]
		if row == nil {
			// SharedSize 0, not -1 (unknown): the docker CLI counts an image with -1 as
			// never reclaimable.
			row = obj{"Id": c.digest, "ParentId": "", "RepoTags": []string{}, "RepoDigests": []string{}, "Created": c.created.Unix(), "Size": c.size, "SharedSize": 0, "Containers": users[c.digest]}
			rows[c.digest] = row
			images = append(images, row)
			layers += c.size
		}
		if !strings.HasPrefix(c.name, "<none>:") {
			row["RepoTags"] = append(row["RepoTags"].([]string), c.name)
			row["RepoDigests"] = append(row["RepoDigests"].([]string), repository(c.name)+"@"+c.digest)
		}
		row["Created"] = min(row["Created"].(int64), c.created.Unix())
	}
	for digest, list := range layered {
		for _, row := range list {
			if image := rows[digest]; image != nil {
				row["SizeRootFs"] = row["SizeRw"].(int64) + image["Size"].(int64)
			}
		}
	}
	volumes := []any{}
	for _, name := range b.volumeNames() {
		_ = safeRun(func() {
			info := b.volumeInfo(name)
			info["UsageData"] = obj{"Size": diskSize(volumeData(b.Home, name)), "RefCount": mounts[name]}
			volumes = append(volumes, info)
		})
	}
	return obj{"LayersSize": layers, "Images": images, "Containers": containers, "Volumes": volumes, "BuildCache": b.buildCacheEntries()}
}

// cachedImage is one cached image name (a directory of the image cache).
type cachedImage struct {
	dir, name, digest string
	size              int64
	created           time.Time
}

func (b *engine) cachedImages() []cachedImage {
	entries, e := os.ReadDir(b.Cache)
	check(e)
	out := []cachedImage{}
	for _, entry := range entries {
		if !entry.IsDir() || len(entry.Name()) != 64 {
			continue
		}
		_ = safeRun(func() {
			p := filepath.Join(b.Cache, entry.Name(), "image.json")
			meta := readObject(p)
			st, e := os.Stat(p)
			check(e)
			c := cachedImage{dir: filepath.Dir(p), name: str(meta["name"]), digest: str(meta["digest"]), created: st.ModTime()}
			if meta["size"] != nil {
				c.size = integer(meta["size"])
			}
			out = append(out, c)
		})
	}
	return out
}

// pruneImages removes the images no container was created from: untagged ones
// ("<none>:ID"), or all of them unless danglingOnly, as keep allows.
func (b *engine) pruneImages(danglingOnly bool, keep func(time.Time, obj) bool) obj {
	used := map[string]bool{}
	for _, name := range b.names() {
		_ = safeRun(func() { used[str(readObject(filepath.Join(b.container(name), "config.json"))["image"])] = true })
	}
	for _, c := range b.creating {
		used[str(c["image"])] = true
	}
	groups, order := map[string][]cachedImage{}, []string{}
	for _, c := range b.cachedImages() {
		if groups[c.digest] == nil {
			order = append(order, c.digest)
		}
		groups[c.digest] = append(groups[c.digest], c)
	}
	deleted, reclaimed := []any{}, int64(0)
	for _, d := range order {
		tagged := slices.ContainsFunc(groups[d], func(c cachedImage) bool { return !strings.HasPrefix(c.name, "<none>:") })
		if used[d] || danglingOnly && tagged { // an image with a tag is not dangling
			continue
		}
		removed := 0
		for _, c := range groups[d] {
			labels := obj{}
			_ = safeRun(func() {
				_, config := imageDocs(noLinks(c.dir, "layout"))
				labels = object(object(config["config"])["Labels"])
			})
			if !keep(c.created, labels) {
				continue
			}
			retainLowers(b.Home, b.State, c.dir)
			check(os.RemoveAll(c.dir))
			removed++
			if !strings.HasPrefix(c.name, "<none>:") {
				deleted = append(deleted, obj{"Untagged": c.name})
			}
		}
		if removed == len(groups[d]) {
			deleted = append(deleted, obj{"Deleted": d})
			reclaimed += groups[d][0].size
		}
	}
	return obj{"ImagesDeleted": deleted, "SpaceReclaimed": reclaimed}
}

// pruneFilter reads a prune request's filters: until (a duration before now, a Unix
// time or an RFC 3339 date), label (KEY or KEY=VALUE) and label! (lacking it),
// plus the extra keys the caller handles; others are refused. keep reports whether
// an object created then, with these labels, may be pruned.
func pruneFilter(q url.Values, extra ...string) (filters obj, keep func(created time.Time, labels obj) bool) {
	filters = obj{}
	if f := q.Get("filters"); f != "" {
		filters = decode([]byte(f), maxJSON)
	}
	reject(onlyKeys(filters, append([]string{"until", "label", "label!"}, extra...)...), "Unsupported prune filter: until, label and label! are supported")
	var until time.Time
	values := filterValues(filters["until"])
	reject(len(values) <= 1, "Give one until filter")
	for _, v := range values {
		until = untilTime(v, time.Now())
	}
	with, without := filterValues(filters["label"]), filterValues(filters["label!"])
	return filters, func(created time.Time, labels obj) bool {
		if !until.IsZero() && !created.Before(until) || !labelsMatch(with, labels) {
			return false
		}
		for _, l := range without {
			if labelsMatch([]string{l}, labels) {
				return false
			}
		}
		return true
	}
}

func untilTime(v string, now time.Time) time.Time {
	if d, e := time.ParseDuration(v); e == nil {
		return now.Add(-d)
	}
	if f, e := strconv.ParseFloat(v, 64); e == nil {
		return time.Unix(0, int64(f*1e9))
	}
	for _, layout := range []string{time.RFC3339Nano, "2006-01-02T15:04:05", "2006-01-02"} {
		if t, e := time.ParseInLocation(layout, v, time.Local); e == nil {
			return t
		}
	}
	panic(apiError{400, "Invalid until filter (a duration, Unix time or RFC 3339 date): " + v})
}

// stopAll stops every running container, as docker stop does (its stop signal and
// grace period), all at once. The supervisor marks a stopped container as stopped
// by the user; that mark is removed, so restartOnBoot starts "always" and
// "unless-stopped" containers again with the next engine.
func (b *engine) stopAll() {
	var wg sync.WaitGroup
	for _, name := range b.names() {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if e := safeRun(func() {
				p := b.container(name)
				if !live(b.status(p)) {
					return
				}
				b.stop(name)
				f, e := settledContainerLock(p) // the supervisor has finished with status.json
				check(e)
				defer f.Close()
				s := readStatus(p)
				delete(s, "stopped_by_user")
				save(filepath.Join(p, "status.json"), s)
			}); e != nil {
				fmt.Fprintln(os.Stderr, "dockivad: shutdown: stopping "+name+":", e)
			}
		}()
	}
	wg.Wait()
}
