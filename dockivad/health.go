package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

// Healthchecks (Config.Healthcheck, or the image's HEALTHCHECK): the supervisor runs
// the test beside the container, as docker exec does, and records State.Health in
// health.json. Restart policies are also the supervisor's (see supervise).

// healthcheck is the saved form of a healthcheck: the test as argv and durations in
// nanoseconds, with Docker's defaults. nil: none (or "NONE").
func healthcheck(given, image obj) obj {
	h := given
	if len(stringsOf(h["Test"])) == 0 {
		h = image
	}
	test := stringsOf(h["Test"])
	if len(test) == 0 || test[0] == "NONE" {
		return nil
	}
	var argv []string
	switch test[0] {
	case "CMD":
		argv = test[1:]
	case "CMD-SHELL":
		argv = []string{"/bin/sh", "-c", strings.Join(test[1:], " ")}
	default:
		panic(apiError{400, "Healthcheck test must start with CMD, CMD-SHELL or NONE"})
	}
	reject(len(argv) > 0 && argv[0] != "", "Empty healthcheck command")
	duration := func(k string, fallback time.Duration) int64 {
		for _, from := range []obj{given, image} {
			if !idle(from[k]) {
				d := integer(from[k])
				reject(d >= int64(time.Millisecond) && d <= int64(24*time.Hour), "Healthcheck "+k+" must be 1ms to 24h")
				return d
			}
		}
		return int64(fallback)
	}
	retries := int64(3)
	for _, from := range []obj{image, given} {
		if !idle(from["Retries"]) {
			retries = integer(from["Retries"])
		}
	}
	reject(retries >= 1 && retries <= 1000, "Healthcheck Retries must be 1 to 1000")
	return obj{"test": argv, "interval": duration("Interval", 30*time.Second), "timeout": duration("Timeout", 30*time.Second),
		"start_period": duration("StartPeriod", 0), "start_interval": duration("StartInterval", 5*time.Second), "retries": retries}
}

// probe runs one healthcheck and returns its exit code (-1: timed out) and output.
func probe(p, native, worker string, c obj, argv []string, timeout time.Duration) (code int, output string) {
	defer func() {
		if v := recover(); v != nil {
			code, output = 127, "healthcheck: "+strings.TrimPrefix(strings.TrimPrefix(errorText(v), "exec: "), "Error: ")
		}
	}()
	env := stringsOf(c["env"])
	command := append([]string{lookPath(noLinks(p, "rootfs"), env, argv[0])}, argv[1:]...)
	cmd := exec.Command(worker, runtimeCommand(p, native, c, command, argv[0], env, false)...)
	alive, keep, e := os.Pipe()
	check(e)
	defer keep.Close()
	out := &limited{max: 4096}
	cmd.ExtraFiles, cmd.Stdout, cmd.Stderr = []*os.File{alive}, out, out
	cmd.Env = []string{"PATH=/usr/bin:/bin", "LANG=C"}
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	check(cmd.Start())
	alive.Close()
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case <-done:
		return cmd.ProcessState.ExitCode(), string(out.data)
	case <-time.After(timeout):
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		<-done
		return -1, "Health check exceeded timeout (" + timeout.String() + ")"
	}
}

func errorText(v any) string {
	if e, ok := v.(apiError); ok {
		return e.Message
	}
	return "failed"
}

type limited struct {
	data []byte
	max  int
}

func (l *limited) Write(b []byte) (int, error) {
	if room := l.max - len(l.data); room > 0 {
		l.data = append(l.data, b[:min(room, len(b))]...)
	}
	return len(b), nil
}

// watchHealth runs the healthcheck until stop closes: every start_interval during
// the start period (where failures do not count), then every interval.
func watchHealth(p, native, worker string, c obj, stop <-chan struct{}) {
	hc := object(c["healthcheck"])
	argv := stringsOf(hc["test"])
	interval, timeout := time.Duration(integer(hc["interval"])), time.Duration(integer(hc["timeout"]))
	startPeriod, startInterval := time.Duration(integer(hc["start_period"])), time.Duration(integer(hc["start_interval"]))
	retries := integer(hc["retries"])
	started := time.Now()
	health := obj{"Status": "starting", "FailingStreak": 0, "Log": []any{}}
	path := filepath.Join(p, "health.json")
	save(path, health)
	streak := int64(0)
	for {
		wait := interval
		if time.Since(started) < startPeriod && health["Status"] == "starting" {
			wait = startInterval
		}
		select {
		case <-stop:
			return
		case <-time.After(wait):
		}
		begin := time.Now()
		code, output := probe(p, native, worker, c, argv, timeout)
		select {
		case <-stop: // the run ended meanwhile: its result says nothing
			return
		default:
		}
		switch {
		case code == 0:
			streak = 0
			health["Status"] = "healthy"
		case time.Since(started) < startPeriod && health["Status"] == "starting":
		default:
			streak++
			if streak >= retries {
				health["Status"] = "unhealthy"
			}
		}
		entry := obj{"Start": begin.UTC().Format(time.RFC3339Nano), "End": time.Now().UTC().Format(time.RFC3339Nano), "ExitCode": code, "Output": output}
		log := append(array(health["Log"]), entry)
		if len(log) > 5 {
			log = log[len(log)-5:]
		}
		health["Log"], health["FailingStreak"] = log, streak
		_ = safeRun(func() { save(path, health) })
	}
}

// restartDelay decides whether a run that ended with code restarts under the
// container's policy, and after how long: Docker's 100 ms, doubling to a minute,
// back to 100 ms after a run of 10 s or more.
func restartDelay(c obj, code, restarts int, ran, previous time.Duration) (time.Duration, bool) {
	policy := object(c["restart"])
	switch policy["name"] {
	case "always", "unless-stopped":
	case "on-failure":
		if code == 0 || !idle(policy["max"]) && integer(policy["max"]) > 0 && int64(restarts) >= integer(policy["max"]) {
			return 0, false
		}
	default:
		return 0, false
	}
	if ran >= 10*time.Second || previous == 0 {
		return 100 * time.Millisecond, true
	}
	return min(previous*2, time.Minute), true
}
