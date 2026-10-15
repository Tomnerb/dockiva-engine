package main

// Published ports (docker run -p, -P): validation, how inspect and ps show them, and
// the forwarders a container's supervisor runs while the container is up.

import (
	"cmp"
	"errors"
	"io"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// portBinding is one published port: -p [IP:][HOST:]GUEST[/PROTO].
type portBinding struct {
	Guest, Host int64  // Host 0: a free port, picked as the container starts (-P, -p GUEST)
	Proto, IP   string // tcp or udp; IP "": the engine's default (-default-bind-ip)
}

func (p portBinding) key() string { return strconv.FormatInt(p.Guest, 10) + "/" + p.Proto }

// portKey parses a Docker port key, "80", "80/tcp" or "53/udp".
func portKey(k string) (int64, string) {
	port, proto, _ := strings.Cut(k, "/")
	proto = cmp.Or(proto, "tcp")
	if proto != "tcp" && proto != "udp" {
		panic(apiError{501, "Only TCP and UDP ports are supported: " + k})
	}
	n, e := strconv.ParseInt(port, 10, 32)
	reject(e == nil && n >= 1 && n <= 65535, "Invalid container port: "+k)
	return n, proto
}

// portBindings validates Docker PortBindings, {"80/tcp":[{"HostIp":"","HostPort":"8080"}]},
// as saved [container, host, protocol, host IP] entries. -p 8000-8005:8000-8005
// arrives as one key per port.
func portBindings(v any) []any {
	m := object(v)
	out, seen := []any{}, map[string]bool{}
	for k, raw := range m {
		guest, proto := portKey(k)
		bindings := array(raw)
		reject(len(bindings) <= 16, "Too many bindings for "+k)
		for _, item := range bindings {
			b := object(item)
			reject(onlyKeys(b, "HostIp", "HostPort"), "Unsupported port binding option")
			ip := strings.Trim(text(b["HostIp"]), "[]")
			if ip != "" {
				a, e := netip.ParseAddr(ip)
				reject(e == nil && a.Zone() == "", "Invalid host IP: "+ip)
				ip = a.Unmap().String()
			}
			host := int64(0)
			if hp := text(b["HostPort"]); hp != "" {
				reject(!strings.Contains(hp, "-"), "Host port ranges for one container port are not supported: "+hp)
				var e error
				host, e = strconv.ParseInt(hp, 10, 32)
				reject(e == nil && host >= 0 && host <= 65535, "Invalid host port: "+hp)
			}
			id := proto + " " + ip + " " + strconv.FormatInt(host, 10)
			reject(host == 0 || !seen[id], "Duplicate published port: "+id)
			seen[id] = true
			out = append(out, []any{guest, host, proto, ip})
		}
	}
	reject(len(out) <= 64, "Too many published ports")
	sortPorts(out)
	return out
}

// publishAll adds -P's bindings: each exposed port not already published, to a free port.
func publishAll(ports []any, exposed []string) []any {
	published := map[string]bool{}
	for _, p := range savedPorts(obj{"ports": ports}) {
		published[p.key()] = true
	}
	for _, k := range exposed {
		guest, proto := portKey(k)
		if !published[k] {
			ports = append(ports, []any{guest, int64(0), proto, ""})
		}
	}
	reject(len(ports) <= 64, "Too many published ports")
	sortPorts(ports)
	return ports
}

func sortPorts(out []any) {
	slices.SortStableFunc(out, func(a, b any) int {
		x, y := a.([]any), b.([]any)
		if x[0].(int64) != y[0].(int64) {
			return int(x[0].(int64) - y[0].(int64))
		}
		return strings.Compare(x[2].(string)+x[3].(string), y[2].(string)+y[3].(string))
	})
}

// savedPorts reads a config's published ports. Containers created before host IPs
// and UDP saved [container, host] pairs, published on the loopback.
func savedPorts(c obj) []portBinding {
	out := []portBinding{}
	if c["ports"] == nil {
		return out
	}
	for _, v := range array(c["ports"]) {
		entry := array(v)
		reject(len(entry) == 2 || len(entry) == 4, "Invalid saved port")
		p := portBinding{Guest: number(entry[0]), Host: number(entry[1]), Proto: "tcp", IP: "127.0.0.1"}
		if len(entry) == 4 {
			p.Proto, p.IP = str(entry[2]), str(entry[3])
		}
		out = append(out, p)
	}
	return out
}

// number is a saved integer: json.Number from disk, int64 before saving.
func number(v any) int64 {
	if n, ok := v.(int64); ok {
		return n
	}
	return integer(v)
}

// exposedPorts is Config.ExposedPorts: the image's and docker run --expose's, and
// every published port.
func exposedPorts(c obj) obj {
	out := obj{}
	if c["exposed"] != nil {
		for _, k := range array(c["exposed"]) {
			out[str(k)] = obj{}
		}
	}
	for _, p := range savedPorts(c) {
		out[p.key()] = obj{}
	}
	return out
}

// hostPortBindings is HostConfig.PortBindings: the bindings as asked for.
func hostPortBindings(c obj) obj {
	out := obj{}
	for _, p := range savedPorts(c) {
		host := ""
		if p.Host != 0 {
			host = strconv.FormatInt(p.Host, 10)
		}
		list, _ := out[p.key()].([]any)
		out[p.key()] = append(list, obj{"HostIp": p.IP, "HostPort": host})
	}
	return out
}

// networkPortBindings is NetworkSettings.Ports: while the container runs, where each
// exposed port is published (the addresses and ports its supervisor bound, from
// ports.json), or null when it is not; empty otherwise, as in Docker.
func networkPortBindings(p string, c obj, running bool) obj {
	out := obj{}
	if !running {
		return out
	}
	for k := range exposedPorts(c) {
		out[k] = nil
	}
	path := filepath.Join(p, "ports.json")
	if !exists(path) {
		return out
	}
	for _, v := range array(readObject(path)["ports"]) {
		e := object(v)
		k := str(e["key"])
		list, _ := out[k].([]any)
		out[k] = append(list, obj{"HostIp": str(e["ip"]), "HostPort": strconv.FormatInt(integer(e["port"]), 10)})
	}
	return out
}

// portRows lists an inspected container's ports as /containers/json rows: one per
// binding, and the exposed ports with none.
func portRows(item obj) []any {
	rows := []any{}
	for k, v := range object(object(item["NetworkSettings"])["Ports"]) {
		guest, proto := portKey(k)
		list, _ := v.([]any)
		if len(list) == 0 {
			rows = append(rows, obj{"PrivatePort": guest, "Type": proto})
		}
		for _, b := range list {
			host, _ := strconv.ParseInt(str(b.(obj)["HostPort"]), 10, 32)
			rows = append(rows, obj{"IP": b.(obj)["HostIp"], "PrivatePort": guest, "PublicPort": host, "Type": proto})
		}
	}
	slices.SortStableFunc(rows, func(a, b any) int {
		x, y := a.(obj), b.(obj)
		if x["PrivatePort"].(int64) != y["PrivatePort"].(int64) {
			return int(x["PrivatePort"].(int64) - y["PrivatePort"].(int64))
		}
		return strings.Compare(text(x["Type"])+text(x["IP"]), text(y["Type"])+text(y["IP"]))
	})
	return rows
}

// udpIdle ends a UDP forwarding session after this long without a datagram either
// way, as Docker's userland proxy does.
const udpIdle = 90 * time.Second

// publishPorts opens a starting container's published ports and returns the
// runtime's --publish options and a function that closes them. A container port the
// runtime binds on the host loopback under another port (--publish GUEST:INTERNAL);
// each binding is a listener on its host address that forwards there. A single TCP
// binding on 127.0.0.1 skips the forwarder: the runtime binds that port itself.
// ports.json records what was bound, for inspect, ps and docker port.
func publishPorts(p string, c obj, defaultIP string) (args []string, closeAll func()) {
	defaultIP = cmp.Or(defaultIP, "127.0.0.1")
	byGuest := map[int64][]portBinding{}
	guests := []int64{}
	for _, b := range savedPorts(c) {
		if byGuest[b.Guest] == nil {
			guests = append(guests, b.Guest)
		}
		byGuest[b.Guest] = append(byGuest[b.Guest], b)
	}
	reject(len(guests) <= 32, "At most 32 container ports can be published")
	var closers []io.Closer
	closeAll = func() {
		for _, c := range closers {
			c.Close()
		}
		_ = os.Remove(filepath.Join(p, "ports.json"))
	}
	ok := false
	defer func() {
		if !ok {
			closeAll()
		}
	}()
	bound := []any{}
	record := func(b portBinding, ip string, port int) {
		bound = append(bound, obj{"key": b.key(), "ip": ip, "port": port})
	}
	for _, guest := range guests {
		list := byGuest[guest]
		if len(list) == 1 && list[0].Proto == "tcp" && cmp.Or(list[0].IP, defaultIP) == "127.0.0.1" && list[0].Host != 0 {
			args = append(args, "--publish", strconv.FormatInt(guest, 10)+":"+strconv.FormatInt(list[0].Host, 10))
			record(list[0], "127.0.0.1", int(list[0].Host))
			continue
		}
		internal := internalPort()
		args = append(args, "--publish", strconv.FormatInt(guest, 10)+":"+strconv.Itoa(internal))
		target := netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), uint16(internal))
		for _, b := range list {
			// The default 0.0.0.0 is Docker's 0.0.0.0 and ::, one dual-stack socket here;
			// an explicit 0.0.0.0 or :: is that family alone.
			ip := cmp.Or(b.IP, defaultIP)
			network, ips := b.Proto, []string{ip}
			address := net.JoinHostPort(ip, strconv.FormatInt(b.Host, 10))
			switch {
			case b.IP == "" && ip == "0.0.0.0":
				ips, address = []string{"0.0.0.0", "::"}, ":"+strconv.FormatInt(b.Host, 10)
			case ip == "0.0.0.0":
				network += "4"
			case ip == "::":
				network += "6"
			}
			var port int
			if b.Proto == "udp" {
				l, e := net.ListenPacket(network, address)
				if e != nil {
					panic(apiError{409, "Bind for " + net.JoinHostPort(ip, strconv.FormatInt(b.Host, 10)) + "/udp failed: " + e.Error()})
				}
				closers = append(closers, l)
				port = l.LocalAddr().(*net.UDPAddr).Port
				go forwardUDP(l.(*net.UDPConn), target)
			} else {
				l, e := net.Listen(network, address)
				if e != nil {
					panic(apiError{409, "Bind for " + net.JoinHostPort(ip, strconv.FormatInt(b.Host, 10)) + " failed: " + e.Error()})
				}
				closers = append(closers, l)
				port = l.Addr().(*net.TCPAddr).Port
				go forwardTCP(l, target.String())
			}
			for _, ip := range ips {
				record(b, ip, port)
			}
		}
	}
	save(filepath.Join(p, "ports.json"), obj{"ports": bound})
	ok = true
	return args, closeAll
}

// internalPort finds a host loopback port free for both TCP and UDP.
// ponytail: it is free when checked; another program could take it before the
// container binds it (the bind then fails in the container). Passing the runtime a
// bound socket would close that window.
func internalPort() int {
	for range 64 {
		l, e := net.Listen("tcp4", "127.0.0.1:0")
		check(e)
		port := l.Addr().(*net.TCPAddr).Port
		u, e := net.ListenPacket("udp4", "127.0.0.1:"+strconv.Itoa(port))
		l.Close()
		if e == nil {
			u.Close()
			return port
		}
	}
	panic(apiError{409, "No free loopback port to publish on"})
}

// forwardTCP relays each connection to target until the listener closes, with
// half-closes passed on (a client that ends its request still reads the reply).
func forwardTCP(l net.Listener, target string) {
	for {
		client, e := l.Accept()
		if errors.Is(e, net.ErrClosed) {
			return
		}
		if e != nil { // out of descriptors, say: try again shortly
			time.Sleep(10 * time.Millisecond)
			continue
		}
		go func() {
			defer client.Close()
			server, e := net.DialTimeout("tcp", target, 5*time.Second)
			if e != nil {
				return
			}
			defer server.Close()
			done := make(chan struct{})
			go func() {
				_, _ = io.Copy(server, client)
				_ = server.(*net.TCPConn).CloseWrite()
				close(done)
			}()
			_, _ = io.Copy(client, server)
			_ = client.(*net.TCPConn).CloseWrite()
			<-done
		}()
	}
}

// forwardUDP relays datagrams to target: each client (source address) gets its own
// socket towards the container, so replies go back to that client, until udpIdle
// passes without traffic. At most 1024 clients at once; others are dropped.
func forwardUDP(l *net.UDPConn, target netip.AddrPort) {
	type session struct {
		conn *net.UDPConn
		last atomic.Int64 // Unix nanoseconds of the latest datagram either way
	}
	var mu sync.Mutex
	sessions := map[netip.AddrPort]*session{}
	defer func() {
		mu.Lock()
		defer mu.Unlock()
		for _, s := range sessions {
			s.conn.Close()
		}
	}()
	buf := make([]byte, 65535)
	for {
		n, from, e := l.ReadFromUDPAddrPort(buf)
		if errors.Is(e, net.ErrClosed) {
			return
		}
		if e != nil {
			continue
		}
		mu.Lock()
		s := sessions[from]
		if s == nil && len(sessions) < 1024 {
			conn, e := net.DialUDP("udp4", nil, net.UDPAddrFromAddrPort(target))
			if e == nil {
				s = &session{conn: conn}
				s.last.Store(time.Now().UnixNano())
				sessions[from] = s
				go func() { // the container's replies, until the session idles out
					defer func() {
						mu.Lock()
						delete(sessions, from)
						mu.Unlock()
						conn.Close()
					}()
					reply := make([]byte, 65535)
					for {
						_ = conn.SetReadDeadline(time.Unix(0, s.last.Load()).Add(udpIdle))
						n, e := conn.Read(reply)
						var timeout net.Error
						if errors.As(e, &timeout) && timeout.Timeout() && time.Since(time.Unix(0, s.last.Load())) < udpIdle {
							continue // traffic the other way kept it alive
						}
						if e != nil {
							return
						}
						s.last.Store(time.Now().UnixNano())
						_, _ = l.WriteToUDPAddrPort(reply[:n], from)
					}
				}()
			}
		}
		mu.Unlock()
		if s != nil {
			s.last.Store(time.Now().UnixNano())
			_, _ = s.conn.Write(buf[:n])
		}
	}
}
