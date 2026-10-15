package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// docker logs merges stdout and stderr in the order lines were written (their
// indexes), --tail counts lines of both, and logs from before indexes still work.
func TestInterleavedLogs(t *testing.T) {
	b := testEngine(t)
	h := b.socketHandler()
	if r := request(t, h, "POST", "/containers/create?name=mixed", `{"Image":"dockiva/shell:demo"}`, ""); r.Code != 201 {
		t.Fatalf("create: %d %s", r.Code, r.Body.String())
	}
	p := b.container("mixed")
	// stdout lines at 1000 s and 3000 s, stderr lines at 2000 s and 4000 s.
	for i, stream := range []string{"stdout", "stderr"} {
		l := openStampedLog(p, stream)
		_, e := l.Write([]byte(stream[3:] + "1\n" + stream[3:] + "2\n"))
		check(e)
		check(l.Close())
		idx, e := os.ReadFile(filepath.Join(p, stream+".idx"))
		check(e)
		for line := range 2 {
			binary.LittleEndian.PutUint64(idx[line*16+8:], uint64(1000*(2*line+i+1))*1e9)
		}
		check(os.WriteFile(filepath.Join(p, stream+".idx"), idx, 0600))
	}
	logs := func(query string) string { // frames as "1:data" / "2:data"
		r := request(t, h, "GET", "/containers/mixed/logs?stdout=1&stderr=1&"+query, "", "")
		if r.Code != 200 {
			t.Fatalf("%s: %d %s", query, r.Code, r.Body.String())
		}
		out := []string{}
		for raw := r.Body.Bytes(); len(raw) >= 8; raw = raw[8+binary.BigEndian.Uint32(raw[4:]):] {
			out = append(out, strconv.Itoa(int(raw[0]))+":"+string(raw[8:8+binary.BigEndian.Uint32(raw[4:])]))
		}
		return strings.Join(out, "|")
	}
	for query, want := range map[string]string{
		"":                        "1:out1\n|2:err1\n|1:out2\n|2:err2\n",
		"tail=2":                  "1:out2\n|2:err2\n",
		"tail=3&since=1500":       "2:err1\n|1:out2\n|2:err2\n",
		"timestamps=1&until=2500": "1:1970-01-01T00:16:40.000000000Z out1\n|2:1970-01-01T00:33:20.000000000Z err1\n",
		"tail=0":                  "",
	} {
		if got := logs(query); got != want {
			t.Errorf("%q: %q, want %q", query, got, want)
		}
	}
	// Written by an older engine: no indexes, so each line takes its log's last write.
	for stream, when := range map[string]int64{"stdout": 5000, "stderr": 4500} {
		check(os.Remove(filepath.Join(p, stream+".idx")))
		check(os.Chtimes(filepath.Join(p, stream+".log"), time.Unix(when, 0), time.Unix(when, 0)))
	}
	if got := logs(""); got != "2:err1\nerr2\n|1:out1\nout2\n" {
		t.Errorf("unindexed: %q", got)
	}
	if got := logs("tail=1&timestamps=1"); got != "1:1970-01-01T01:23:20.000000000Z out2\n" {
		t.Errorf("unindexed tail: %q", got)
	}
}

// docker events: --since replays kept events, --until ends the stream, and filters
// select by type, event, container, image and label.
func TestEvents(t *testing.T) {
	b := testEngine(t)
	h := b.socketHandler()
	b.watchPass() // the engine's first pass, as it starts
	begin := time.Now()
	for _, step := range []struct{ method, path, body string }{
		{"POST", "/containers/create?name=ev", `{"Image":"dockiva/shell:demo","Labels":{"app":"web"}}`},
		{"POST", "/containers/ev/rename?name=ev2", ""},
		{"POST", "/containers/ev2/update", `{"RestartPolicy":{"Name":"always"}}`},
		{"POST", "/images/dockiva/shell:demo/tag?repo=test/ev&tag=1", ""},
		{"DELETE", "/images/test/ev:1", ""},
		{"DELETE", "/containers/ev2", ""},
	} {
		if r := request(t, h, step.method, step.path, step.body, ""); r.Code >= 300 {
			t.Fatalf("%s %s: %d %s", step.method, step.path, r.Code, r.Body.String())
		}
	}
	events := func(filters string) []obj {
		query := url.Values{"since": {strconv.FormatInt(begin.Unix()-1, 10)}, "until": {strconv.FormatFloat(float64(time.Now().UnixNano())/1e9, 'f', 9, 64)}}
		if filters != "" {
			query.Set("filters", filters)
		}
		done := make(chan []obj)
		go func() { // until is past: the stream ends after the kept events
			r := request(t, h, "GET", "/events?"+query.Encode(), "", "")
			out := []obj{}
			for s := bufio.NewScanner(r.Body); s.Scan(); {
				ev := obj{}
				check(json.Unmarshal(s.Bytes(), &ev))
				out = append(out, ev)
			}
			done <- out
		}()
		select {
		case out := <-done:
			return out
		case <-time.After(5 * time.Second):
			t.Fatal("events did not end at until")
			return nil
		}
	}
	actions := func(list []obj) string {
		out := []string{}
		for _, ev := range list {
			out = append(out, str(ev["Type"])+" "+str(ev["Action"]))
		}
		return strings.Join(out, ", ")
	}
	all := events("")
	if got := actions(all); got != "container create, container rename, container update, image tag, image untag, container destroy" {
		t.Fatalf("events: %s", got)
	}
	rename := object(object(all[1]["Actor"])["Attributes"])
	if all[1]["id"] != key("ev") || rename["oldName"] != "/ev" || rename["name"] != "ev2" || rename["image"] != "dockiva/shell:demo" || rename["app"] != "web" {
		t.Fatalf("rename event: %v", all[1])
	}
	for filters, want := range map[string]string{
		`{"type":["image"]}`:                         "image tag, image untag",
		`{"event":["rename","destroy"]}`:             "container rename, container destroy",
		`{"container":["ev2"]}`:                      "container rename, container update, container destroy",
		`{"container":["` + key("ev")[:12] + `"]}`:   "container create, container rename, container update, container destroy",
		`{"label":["app=web"],"event":["create"]}`:   "container create",
		`{"image":["test/ev:1"]}`:                    "image tag",
		`{"type":["container"],"label":["app=api"]}`: "",
	} {
		if got := actions(events(filters)); got != want {
			t.Errorf("%s: %s, want %s", filters, got, want)
		}
	}
	if r := request(t, h, "GET", "/events?filters="+url.QueryEscape(`{"network":["x"]}`), "", ""); r.Code != 400 {
		t.Fatalf("unsupported filter: %d", r.Code)
	}
}
