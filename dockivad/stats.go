package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
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

// top lists a running container's processes (docker top) from the table its run
// publishes: one "pid ppid uid size cmdline" record per process, cmdline being
// size bytes of NUL-separated arguments, then "=" lines with usage, of which
// "=pid utime stime ..." gives each process's CPU time in ticks (1/100 s).
func (b *engine) top(name string) obj {
	item := b.inspect(name)
	state := object(item["State"])
	if state["Running"] != true {
		panic(apiError{409, "Container " + name + " is not running"})
	}
	stime := "?"
	if started, e := time.Parse(time.RFC3339Nano, str(state["StartedAt"])); e == nil {
		stime = started.Local().Format("15:04")
	}
	data, _ := os.ReadFile(filepath.Join(b.container(name), "procs"))
	return obj{"Titles": []string{"UID", "PID", "PPID", "C", "STIME", "TTY", "TIME", "CMD"}, "Processes": topRows(data, stime)}
}

func topRows(data []byte, stime string) [][]string {
	rows := [][]string{}
	for len(data) > 0 && data[0] != '=' {
		fields := strings.SplitN(string(data[:min(len(data), 96)]), " ", 5)
		if len(fields) < 5 {
			break
		}
		size, e := strconv.Atoi(fields[3])
		header := len(fields[0]) + len(fields[1]) + len(fields[2]) + len(fields[3]) + 4
		if e != nil || size < 0 || header+size >= len(data) {
			break
		}
		cmdline := strings.TrimRight(string(data[header:header+size]), "\x00")
		user := fields[2]
		if user == "0" {
			user = "root"
		}
		rows = append(rows, []string{user, fields[0], fields[1], "0", stime, "?", "00:00:00", strings.ReplaceAll(cmdline, "\x00", " ")})
		data = data[header+size+1:]
	}
	for _, line := range strings.Split(string(data), "\n") {
		f := strings.Fields(strings.TrimPrefix(line, "="))
		if len(f) < 3 {
			continue
		}
		utime, e1 := strconv.ParseInt(f[1], 10, 64)
		stime, e2 := strconv.ParseInt(f[2], 10, 64)
		for _, row := range rows {
			if row[1] == f[0] && e1 == nil && e2 == nil {
				s := (utime + stime) / 100
				row[6] = fmt.Sprintf("%02d:%02d:%02d", s/3600, s/60%60, s%60)
			}
		}
	}
	return rows
}
