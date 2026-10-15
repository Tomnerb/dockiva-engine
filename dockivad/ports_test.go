package main

import (
	"encoding/json"
	"io"
	"net"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
	"time"
)

// -p [IP:][HOST:]GUEST[/udp], ranges (one key per port) and -P: what is saved and
// how inspect and ps show it.
func TestPublishedPorts(t *testing.T) {
	b := testEngine(t)
	b.create("web", decode([]byte(`{"Image":"dockiva/shell:demo","ExposedPorts":{"90/tcp":{},"53/udp":{}},"HostConfig":{"PublishAllPorts":true,"PortBindings":{
		"80/tcp":[{"HostIp":"","HostPort":"8080"},{"HostIp":"127.0.0.1","HostPort":"9080"}],
		"8000/tcp":[{"HostPort":"8000"}],"8001/tcp":[{"HostPort":"8001"}],
		"443/tcp":[{"HostIp":"::","HostPort":"8443"}],"5353/udp":[{"HostIp":"0.0.0.0","HostPort":""}]}}}`), maxJSON))
	p := b.container("web")
	c := readObject(filepath.Join(p, "config.json"))
	got := []string{}
	for _, b := range savedPorts(c) {
		got = append(got, b.key()+"@"+b.IP+":"+strconv.FormatInt(b.Host, 10))
	}
	want := []string{"53/udp@:0", "80/tcp@:8080", "80/tcp@127.0.0.1:9080", "90/tcp@:0", "443/tcp@:::8443", "5353/udp@0.0.0.0:0", "8000/tcp@:8000", "8001/tcp@:8001"}
	if !slices.Equal(got, want) {
		t.Fatalf("saved ports: %v", got)
	}
	item := b.inspect("web")
	host := object(object(item["HostConfig"])["PortBindings"])
	if first := host["80/tcp"].([]any)[0].(obj); len(host) != 7 || first["HostIp"] != "" || first["HostPort"] != "8080" || host["90/tcp"].([]any)[0].(obj)["HostPort"] != "" {
		t.Fatalf("PortBindings: %v", host)
	}
	if ports := object(object(item["NetworkSettings"])["Ports"]); len(ports) != 0 { // not running
		t.Fatalf("NetworkSettings.Ports of a created container: %v", ports)
	}
	if exposed := object(object(item["Config"])["ExposedPorts"]); len(exposed) != 7 || exposed["53/udp"] == nil {
		t.Fatalf("ExposedPorts: %v", exposed)
	}
	// Running: what the supervisor bound (ports.json), and exposed ports it did not.
	save(filepath.Join(p, "ports.json"), obj{"ports": []any{obj{"key": "80/tcp", "ip": "0.0.0.0", "port": 8080}, obj{"key": "80/tcp", "ip": "::", "port": 8080}}})
	c["exposed"] = []any{"80/tcp", "22/tcp"}
	c["ports"] = []any{[]any{json.Number("80"), json.Number("8080"), "tcp", ""}}
	rows := portRows(obj{"NetworkSettings": obj{"Ports": networkPortBindings(p, c, true)}})
	if len(rows) != 3 || rows[0].(obj)["PrivatePort"] != int64(22) || rows[0].(obj)["IP"] != nil ||
		rows[1].(obj)["IP"] != "0.0.0.0" || rows[2].(obj)["IP"] != "::" || rows[2].(obj)["PublicPort"] != int64(8080) {
		t.Fatalf("ps rows: %v", rows)
	}
	for i, bad := range []string{
		`{"80/sctp":[{"HostPort":"8080"}]}`,
		`{"80/tcp":[{"HostIp":"localhost","HostPort":"8080"}]}`,
		`{"80/tcp":[{"HostPort":"8000-8005"}]}`,
		`{"80/tcp":[{"HostPort":"70000"}]}`,
		`{"80/tcp":[{"HostPort":"8080"}],"81/tcp":[{"HostPort":"8080"}]}`,
		`{"0/tcp":[{"HostPort":"8080"}]}`,
	} {
		mustReject(t, func() {
			b.create("bad"+strconv.Itoa(i), decode([]byte(`{"Image":"dockiva/shell:demo","HostConfig":{"PortBindings":`+bad+`}}`), maxJSON))
		})
	}
	// Containers created before host IPs and UDP: [container, host] pairs on the loopback.
	legacy := obj{"ports": []any{[]any{json.Number("80"), json.Number("8080")}}}
	if got := savedPorts(legacy); len(got) != 1 || got[0] != (portBinding{80, 8080, "tcp", "127.0.0.1"}) || len(savedPorts(obj{})) != 0 {
		t.Fatalf("legacy ports: %v", got)
	}
	if hostPortBindings(legacy)["80/tcp"].([]any)[0].(obj)["HostIp"] != "127.0.0.1" {
		t.Fatal("legacy binding lost its loopback address")
	}
}

// The supervisor's forwarders: TCP on every address by default (another of this
// Mac's addresses, the IPv4 and IPv6 loopbacks), UDP with a session per client, a
// direct runtime binding for one TCP port on 127.0.0.1, and Docker's error for a
// port in use.
func TestPortForwarding(t *testing.T) {
	p := t.TempDir()
	direct := freePort(t)
	c := obj{"ports": portBindings(decode([]byte(`{"80/tcp":[{"HostPort":"0"}],"53/udp":[{"HostIp":"127.0.0.1","HostPort":"0"}],"81/tcp":[{"HostIp":"127.0.0.1","HostPort":"`+strconv.Itoa(direct)+`"}]}`), maxJSON))}
	args, closeAll := publishPorts(p, c, "")
	internal := map[string]string{}
	for i := 0; i+1 < len(args); i += 2 {
		guest, port, _ := strings.Cut(args[i+1], ":")
		internal[guest] = port
	}
	if args[0] != "--publish" || len(internal) != 3 || internal["81"] != strconv.Itoa(direct) {
		t.Fatalf("runtime options: %v", args)
	}
	// The container's servers, as the runtime binds them: echo over TCP and UDP.
	server, e := net.Listen("tcp", "127.0.0.1:"+internal["80"])
	check(e)
	defer server.Close()
	go func() {
		for {
			conn, e := server.Accept()
			if e != nil {
				return
			}
			go func() { _, _ = io.Copy(conn, conn); conn.Close() }()
		}
	}()
	udp, e := net.ListenPacket("udp", "127.0.0.1:"+internal["53"])
	check(e)
	defer udp.Close()
	go func() {
		buf := make([]byte, 1500)
		for {
			n, from, e := udp.ReadFrom(buf)
			if e != nil {
				return
			}
			_, _ = udp.WriteTo(append([]byte("echo "), buf[:n]...), from)
		}
	}()
	bound := map[string]int{}
	for _, v := range array(readObject(filepath.Join(p, "ports.json"))["ports"]) {
		bound[str(object(v)["key"])+"@"+str(object(v)["ip"])] = int(integer(object(v)["port"]))
	}
	tcpPort, udpPort := bound["80/tcp@0.0.0.0"], bound["53/udp@127.0.0.1"]
	if len(bound) != 4 || tcpPort == 0 || bound["80/tcp@::"] != tcpPort || bound["81/tcp@127.0.0.1"] != direct || udpPort == 0 {
		t.Fatalf("bound: %v", bound)
	}
	addresses := []string{"127.0.0.1", "::1"}
	if ip := otherLocalIPv4(); ip != "" {
		addresses = append(addresses, ip)
	} else {
		t.Log("no non-loopback IPv4 address to try")
	}
	for _, a := range addresses {
		conn, e := net.DialTimeout("tcp", net.JoinHostPort(a, strconv.Itoa(tcpPort)), 2*time.Second)
		if e != nil {
			t.Fatalf("%s: %v", a, e)
		}
		_, _ = conn.Write([]byte("hello " + a))
		_ = conn.(*net.TCPConn).CloseWrite() // the reply still comes back
		reply, _ := io.ReadAll(conn)
		conn.Close()
		if string(reply) != "hello "+a {
			t.Fatalf("%s: %q", a, reply)
		}
	}
	for _, client := range []string{"one", "two"} {
		conn, e := net.Dial("udp", "127.0.0.1:"+strconv.Itoa(udpPort))
		check(e)
		_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
		_, _ = conn.Write([]byte(client))
		buf := make([]byte, 64)
		n, e := conn.Read(buf)
		conn.Close()
		if e != nil || string(buf[:n]) != "echo "+client {
			t.Fatalf("udp %s: %q %v", client, buf[:n], e)
		}
	}
	// A port in use: the start fails, as Docker's does.
	busy := obj{"ports": portBindings(decode([]byte(`{"80/tcp":[{"HostPort":"`+strconv.Itoa(tcpPort)+`"}]}`), maxJSON))}
	if e := safeRun(func() { publishPorts(t.TempDir(), busy, "") }); e == nil || !strings.Contains(e.Error(), "Bind for 0.0.0.0:"+strconv.Itoa(tcpPort)+" failed") {
		t.Fatalf("port in use: %v", e)
	}
	closeAll()
	if exists(filepath.Join(p, "ports.json")) {
		t.Fatal("ports.json outlived the run")
	}
	if conn, e := net.DialTimeout("tcp", "127.0.0.1:"+strconv.Itoa(tcpPort), time.Second); e == nil {
		conn.Close()
		t.Fatal("forwarder still listening")
	}
	// -default-bind-ip 127.0.0.1: one TCP binding without an address binds directly.
	args, closeAll = publishPorts(t.TempDir(), obj{"ports": []any{[]any{int64(80), int64(direct), "tcp", ""}}}, "127.0.0.1")
	closeAll()
	if len(args) != 2 || args[1] != "80:"+strconv.Itoa(direct) {
		t.Fatalf("loopback default: %v", args)
	}
}

func freePort(t *testing.T) int {
	l, e := net.Listen("tcp", "127.0.0.1:0")
	check(e)
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

func otherLocalIPv4() string {
	addresses, _ := net.InterfaceAddrs()
	for _, a := range addresses {
		if n, ok := a.(*net.IPNet); ok && n.IP.To4() != nil && !n.IP.IsLoopback() && !n.IP.IsLinkLocalUnicast() {
			return n.IP.String()
		}
	}
	return ""
}
