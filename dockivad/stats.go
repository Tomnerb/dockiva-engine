package main

import (
	"encoding/json"
	"net/http"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"
)

// usage is a container's host CPU time and resident memory: its worker and every
// host process below it (the runtime runs all guest processes inside them).
type usage struct {
	cpu    time.Duration
	memory int64
	at     time.Time
}

func (b *engine) usage(p string) usage {
	u := usage{at: time.Now()}
	s := b.status(p)
	if !live(s) || s["pid"] == nil {
		return u
	}
	out, e := exec.Command("/bin/ps", "-axo", "pid=,ppid=,rss=,time=").Output()
	check(e)
	type row struct {
		parent, rss int64
		cpu         time.Duration
	}
	rows := map[int64]row{}
	for _, line := range strings.Split(string(out), "\n") {
		f := strings.Fields(line)
		if len(f) != 4 {
			continue
		}
		pid, _ := strconv.ParseInt(f[0], 10, 64)
		parent, _ := strconv.ParseInt(f[1], 10, 64)
		rss, _ := strconv.ParseInt(f[2], 10, 64)
		rows[pid] = row{parent, rss, cpuTime(f[3])}
	}
	in := map[int64]bool{integer(s["pid"]): true}
	for changed := true; changed; { // a few levels at most
		changed = false
		for pid, r := range rows {
			if !in[pid] && in[r.parent] {
				in[pid], changed = true, true
			}
		}
	}
	for pid := range in {
		u.cpu += rows[pid].cpu
		u.memory += rows[pid].rss << 10
	}
	return u
}

// cpuTime parses ps's "[[dd-]hh:]mm:ss.cc".
func cpuTime(v string) time.Duration {
	days := 0.0
	if d, rest, ok := strings.Cut(v, "-"); ok {
		days, _ = strconv.ParseFloat(d, 64)
		v = rest
	}
	total := 0.0
	for _, part := range strings.Split(v, ":") {
		n, _ := strconv.ParseFloat(part, 64)
		total = total*60 + n
	}
	return time.Duration((days*86400 + total) * float64(time.Second))
}

// statsJSON is one Docker stats sample. Docker's CPU % is the CPU-time delta over
// system_cpu_usage's delta times online_cpus, so system time is wall time × CPUs.
func statsJSON(name string, c obj, now, before usage) obj {
	cpus := int64(min(runtime.NumCPU(), 64))
	cpu := func(u usage) obj {
		return obj{"cpu_usage": obj{"total_usage": u.cpu.Nanoseconds()}, "system_cpu_usage": u.at.UnixNano() * cpus, "online_cpus": cpus, "throttling_data": obj{}}
	}
	return obj{"id": key(name), "name": "/" + name, "read": now.at.UTC().Format(time.RFC3339Nano), "preread": before.at.UTC().Format(time.RFC3339Nano),
		"cpu_stats": cpu(now), "precpu_stats": cpu(before), "pids_stats": obj{}, "networks": obj{}, "blkio_stats": obj{}, "num_procs": 0,
		"memory_stats": obj{"usage": now.memory, "limit": memoryMiB(c) << 20, "stats": obj{}}}
}

// stats serves docker stats: one sample a second, or one (after a second, so CPU
// has a delta, unless one-shot) when stream is off.
func (b *engine) stats(w http.ResponseWriter, r *http.Request, name string, stream, oneShot bool) {
	p := b.container(name)
	c := readObject(filepath.Join(p, "config.json"))
	before := b.usage(p)
	if !stream {
		if !oneShot {
			time.Sleep(time.Second)
		}
		send(w, 200, statsJSON(name, c, b.usage(p), before))
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(200)
	encoder := json.NewEncoder(w)
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		now := b.usage(p)
		if encoder.Encode(statsJSON(name, c, now, before)) != nil {
			return
		}
		http.NewResponseController(w).Flush()
		before = now
		select {
		case <-r.Context().Done():
			return
		case <-tick.C:
		}
	}
}
