package main

import (
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Networks. "bridge" (the default) and user networks are private: each container
// gets an address in the network's /24 (10.89.N.0/24, the .1 gateway being the Mac)
// and its listening ports become private host ports that the runtime records in
// the network's ports directory. On a user network, containers reach each other by
// name and alias (/etc/hosts); as with Docker, the default bridge has no names.
// "host" shares the Mac's network; "none" keeps a container to its own loopback.
//
// ponytail: one network per container; connecting to a second one is refused.

var builtinNetworks = []string{"bridge", "host", "none"}

func networksDir(home string) string { return private(filepath.Join(home, "networks")) }

func networkPorts(home, name string) string {
	return private(filepath.Join(networksDir(home), name, "ports"))
}

// network loads a network's metadata; the default bridge is created on first use.
func (b *engine) network(name string) obj {
	reject(namePattern.MatchString(name), "Invalid network name")
	p := filepath.Join(networksDir(b.Home), name, "meta.json")
	if !exists(p) && name == "bridge" {
		return b.saveNetwork(name, obj{}, 0)
	}
	if !exists(p) {
		panic(apiError{404, "network " + name + " not found"})
	}
	return readObject(p)
}

func (b *engine) saveNetwork(name string, labels obj, index int) obj {
	dir := filepath.Join(networksDir(b.Home), name)
	check(os.MkdirAll(dir, 0o700))
	meta := obj{"name": name, "id": token(), "subnet": fmt.Sprintf("10.89.%d.0/24", index), "created": now(), "labels": labels}
	save(filepath.Join(dir, "meta.json"), meta)
	networkPorts(b.Home, name)
	return readObject(filepath.Join(dir, "meta.json"))
}

func (b *engine) networkNames() []string {
	entries, e := os.ReadDir(networksDir(b.Home))
	check(e)
	out := []string{}
	for _, entry := range entries {
		if entry.IsDir() && namePattern.MatchString(entry.Name()) && exists(filepath.Join(networksDir(b.Home), entry.Name(), "meta.json")) {
			out = append(out, entry.Name())
		}
	}
	return out
}

// resolveNetwork finds a network by name, or ID (prefix).
func (b *engine) resolveNetwork(id string) string {
	for _, n := range builtinNetworks {
		if id == n {
			return n
		}
	}
	names := b.networkNames()
	for _, n := range names {
		if n == id {
			return n
		}
	}
	match := ""
	for _, n := range names {
		if nid := str(b.network(n)["id"]); len(id) >= 4 && strings.HasPrefix(nid, id) {
			reject(match == "", "Ambiguous network ID: "+id)
			match = n
		}
	}
	if match == "" {
		panic(apiError{404, "network " + id + " not found"})
	}
	return match
}

func (b *engine) createNetwork(body obj) obj {
	reject(onlyKeys(body, "Name", "Driver", "Labels", "CheckDuplicate", "Internal", "Attachable", "Ingress", "IPAM", "Options", "EnableIPv6", "Scope", "ConfigOnly", "ConfigFrom"), "Unsupported network option")
	reject(idle(body["Driver"]) || body["Driver"] == "bridge", "Only the bridge network driver is supported")
	reject(idle(body["Internal"]) && idle(body["Ingress"]) && idle(body["EnableIPv6"]) && idle(body["ConfigOnly"]) && idle(body["ConfigFrom"]), "Unsupported network option")
	if ipam := object(body["IPAM"]); !idle(ipam["Config"]) {
		panic(apiError{501, "Custom subnets are not supported; networks get 10.89.N.0/24"})
	}
	name := text(body["Name"])
	reject(namePattern.MatchString(name), "Invalid network name")
	for _, n := range builtinNetworks {
		if n == name {
			panic(apiError{409, "network with name " + name + " already exists"})
		}
	}
	if exists(filepath.Join(networksDir(b.Home), name, "meta.json")) {
		panic(apiError{409, "network with name " + name + " already exists"})
	}
	used := map[string]bool{}
	for _, n := range b.networkNames() {
		used[str(b.network(n)["subnet"])] = true
	}
	for i := 1; i < 255; i++ {
		if !used[fmt.Sprintf("10.89.%d.0/24", i)] {
			labels := object(body["Labels"])
			reject(len(labels) <= 64, "Too many labels")
			return obj{"Id": b.saveNetwork(name, labels, i)["id"], "Warning": ""}
		}
	}
	panic(apiError{409, "At most 254 networks"})
}

// members lists the containers on a network: name, address and aliases.
func (b *engine) members(network string) []obj { return membersOn(b.Home, network) }

func membersOn(home, network string) []obj {
	out := []obj{}
	entries, e := os.ReadDir(filepath.Join(home, "containers"))
	check(e)
	for _, entry := range entries {
		config := filepath.Join(home, "containers", entry.Name(), "config.json")
		if !entry.IsDir() || !namePattern.MatchString(entry.Name()) || !exists(config) {
			continue
		}
		if c := readObject(config); c["net_name"] == network {
			out = append(out, obj{"name": entry.Name(), "ip": c["ip"], "aliases": c["aliases"]})
		}
	}
	return out
}

// assignIP gives a container the lowest free address on a network (.2 to .254).
func (b *engine) assignIP(network string) string {
	prefix := strings.TrimSuffix(str(b.network(network)["subnet"]), "0/24")
	used := map[string]bool{}
	for _, m := range b.members(network) {
		used[str(m["ip"])] = true
	}
	for i := 2; i < 255; i++ {
		if ip := prefix + strconv.Itoa(i); !used[ip] {
			return ip
		}
	}
	panic(apiError{409, "Network " + network + " is full (253 containers)"})
}

func gateway(ip string) string { return ip[:strings.LastIndex(ip, ".")] + ".1" }

func gatewayOf(c obj) string {
	if ip, ok := c["ip"].(string); ok {
		return gateway(ip)
	}
	return ""
}

// hostsFile is a container's /etc/hosts: itself, the Mac (host.docker.internal) and,
// on a user network, every other container on it by name and alias.
func hostsFile(home, name string, c obj, members []obj) string {
	ip, _ := c["ip"].(string)
	self := name
	if h := text(c["hostname"]); h != "" && h != name {
		self = h + " " + name
	}
	mac := "127.0.0.1"
	out := "127.0.0.1\tlocalhost\n::1\tlocalhost ip6-localhost ip6-loopback\n127.0.0.1\t" + self + "\n127.0.0.1\thost.docker.internal\n"
	if ip != "" {
		mac = gateway(ip)
		out = "127.0.0.1\tlocalhost\n::1\tlocalhost ip6-localhost ip6-loopback\n" + ip + "\t" + self + "\n" + mac + "\thost.docker.internal gateway.docker.internal\n"
	}
	if c["extra_hosts"] != nil { // --add-host
		for _, v := range array(c["extra_hosts"]) {
			pair := stringsOf(v)
			if pair[1] == "host-gateway" {
				pair[1] = mac
			}
			out += pair[1] + "\t" + pair[0] + "\n"
		}
	}
	if ip == "" || c["net_name"] == "bridge" {
		return out
	}
	for _, m := range members {
		if m["name"] == name {
			continue
		}
		names := []string{str(m["name"])}
		if m["aliases"] != nil {
			names = append(names, stringsOf(m["aliases"])...)
		}
		out += str(m["ip"]) + "\t" + strings.Join(names, " ") + "\n"
	}
	return out
}

// refreshHosts rewrites /etc/hosts in every container on a user network (after one
// joins or leaves); a running container sees the change on its next lookup.
func (b *engine) refreshHosts(network string) {
	if network == "" || network == "bridge" {
		return
	}
	members := b.members(network)
	for _, m := range members {
		name := str(m["name"])
		p := b.container(name)
		c := readObject(filepath.Join(p, "config.json"))
		_ = safeRun(func() { containerLayers(p, c).writeFile("etc/hosts", hostsFile(b.Home, name, c, members)) })
	}
}

// clearPorts forgets a container's recorded ports (its runtime has ended).
func clearPorts(home string, c obj) {
	ip, _ := c["ip"].(string)
	network, _ := c["net_name"].(string)
	if ip == "" || network == "" {
		return
	}
	dir := networkPorts(home, network)
	entries, e := os.ReadDir(dir)
	if e != nil {
		return
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ip+":") {
			_ = os.Remove(filepath.Join(dir, entry.Name()))
		}
	}
}

// containerNetworks is NetworkSettings.Networks for inspect and ps.
func (b *engine) containerNetworks(name string, c obj) obj {
	ip, _ := c["ip"].(string)
	if ip == "" {
		return obj{inspectNetwork(c): obj{"IPAddress": "", "Gateway": "", "IPPrefixLen": 0, "MacAddress": "", "Aliases": nil}}
	}
	network := str(c["net_name"])
	id := ""
	_ = safeRun(func() { id = str(b.network(network)["id"]) })
	return obj{network: obj{"IPAddress": ip, "IPPrefixLen": 24, "Gateway": gateway(ip), "MacAddress": "", "Aliases": c["aliases"], "NetworkID": id, "EndpointID": key(name + "@" + network)}}
}

func (b *engine) networkInfo(name string) obj {
	if name == "host" || name == "none" {
		return obj{"Name": name, "Id": key("network:" + name), "Created": "0001-01-01T00:00:00Z", "Scope": "local", "Driver": map[string]string{"host": "host", "none": "null"}[name],
			"EnableIPv6": false, "IPAM": obj{"Driver": "default", "Config": []any{}}, "Internal": false, "Attachable": false, "Ingress": false, "Containers": obj{}, "Options": obj{}, "Labels": obj{}}
	}
	meta := b.network(name)
	containers := obj{}
	for _, m := range b.members(name) {
		containers[key(str(m["name"]))] = obj{"Name": m["name"], "EndpointID": key(str(m["name"]) + "@" + name), "MacAddress": "", "IPv4Address": str(m["ip"]) + "/24", "IPv6Address": ""}
	}
	subnet := str(meta["subnet"])
	return obj{"Name": name, "Id": meta["id"], "Created": timestamp(meta["created"]), "Scope": "local", "Driver": "bridge", "EnableIPv6": false,
		"IPAM":     obj{"Driver": "default", "Options": nil, "Config": []any{obj{"Subnet": subnet, "Gateway": strings.TrimSuffix(subnet, "0/24") + "1"}}},
		"Internal": false, "Attachable": false, "Ingress": false, "ConfigOnly": false, "Containers": containers, "Options": obj{}, "Labels": object(meta["labels"])}
}

func (b *engine) networksAPI(w http.ResponseWriter, method, path string, q map[string][]string, raw []byte) bool {
	query := func(k string) string {
		if v := q[k]; len(v) > 0 {
			return v[0]
		}
		return ""
	}
	body := func() obj {
		if len(raw) == 0 {
			return obj{}
		}
		return decode(raw, maxJSON)
	}
	switch {
	case path == "/networks" && method == "GET":
		filters := obj{}
		if f := query("filters"); f != "" {
			filters = decode([]byte(f), maxJSON)
		}
		reject(onlyKeys(filters, "name", "id", "label", "driver", "type", "scope"), "Unsupported network filter")
		b.network("bridge")
		names := append(append([]string{}, b.networkNames()...), "host", "none")
		sort.Strings(names)
		rows := []any{}
		for _, name := range names {
			info := b.networkInfo(name)
			keep := true
			if v := filterValues(filters["name"]); len(v) > 0 {
				keep = keep && anyContains(v, name)
			}
			if v := filterValues(filters["id"]); len(v) > 0 {
				keep = keep && anyPrefix(v, str(info["Id"]))
			}
			if v := filterValues(filters["driver"]); len(v) > 0 {
				keep = keep && anyContains(v, str(info["Driver"]))
			}
			if v := filterValues(filters["type"]); len(v) > 0 {
				builtin := name == "bridge" || name == "host" || name == "none"
				keep = keep && ((v[0] == "builtin") == builtin)
			}
			keep = keep && labelsMatch(filterValues(filters["label"]), object(info["Labels"]))
			if keep {
				rows = append(rows, info)
			}
		}
		send(w, 200, rows)
	case path == "/networks/create" && method == "POST":
		b.Mu.Lock()
		defer b.Mu.Unlock()
		send(w, 201, b.createNetwork(body()))
	case path == "/networks/prune" && method == "POST":
		_, keep := pruneFilter(q) // docker system prune passes its until and label filters on
		b.Mu.Lock()
		defer b.Mu.Unlock()
		deleted := []string{}
		for _, name := range b.networkNames() {
			meta := b.network(name)
			created, e := time.Parse(time.RFC3339Nano, timestamp(meta["created"]))
			check(e)
			if name != "bridge" && len(b.members(name)) == 0 && keep(created, object(meta["labels"])) {
				check(os.RemoveAll(filepath.Join(networksDir(b.Home), name)))
				deleted = append(deleted, name)
			}
		}
		send(w, 200, obj{"NetworksDeleted": deleted})
	case strings.HasPrefix(path, "/networks/"):
		rest := strings.TrimPrefix(path, "/networks/")
		id, action, _ := strings.Cut(rest, "/")
		name := b.resolveNetwork(id)
		switch {
		case action == "" && method == "GET":
			send(w, 200, b.networkInfo(name))
		case action == "" && method == "DELETE":
			b.Mu.Lock()
			defer b.Mu.Unlock()
			if name == "bridge" || name == "host" || name == "none" {
				panic(apiError{403, name + " is a pre-defined network and cannot be removed"})
			}
			if m := b.members(name); len(m) > 0 {
				panic(apiError{409, "error while removing network: network " + name + " has active endpoints (" + str(m[0]["name"]) + ")"})
			}
			check(os.RemoveAll(filepath.Join(networksDir(b.Home), name)))
			send(w, 204, nil)
		case (action == "connect" || action == "disconnect") && method == "POST":
			b.Mu.Lock()
			defer b.Mu.Unlock()
			request := body()
			container := b.resolve(text(request["Container"]))
			b.moveNetwork(container, name, action == "connect", object(request["EndpointConfig"]))
			send(w, 200, nil)
		default:
			return false
		}
	default:
		return false
	}
	return true
}

// labelsMatch is Docker's label filter: every "key" or "key=value" holds.
func labelsMatch(filters []string, labels obj) bool {
	for _, l := range filters {
		k, want, has := strings.Cut(l, "=")
		got, ok := labels[k]
		if !ok || has && got != want {
			return false
		}
	}
	return true
}

func anyPrefix(values []string, s string) bool {
	for _, v := range values {
		if strings.HasPrefix(s, v) {
			return true
		}
	}
	return false
}

// moveNetwork connects a stopped container to a network (leaving the default
// bridge), or disconnects it back to the bridge. Already there: nothing to do.
func (b *engine) moveNetwork(name, network string, connect bool, endpoint obj) {
	p := b.container(name)
	path := filepath.Join(p, "config.json")
	c := readObject(path)
	current, _ := c["net_name"].(string)
	if connect && current == network || !connect && current != network {
		return
	}
	reject(network != "host" && network != "none", "Connecting to "+network+" is not supported")
	if live(b.status(p)) {
		panic(apiError{409, "Container " + name + " is running; Dockiva Engine changes networks of stopped containers only"})
	}
	if connect && current != "" && current != "bridge" {
		panic(apiError{501, "Container " + name + " is already on network " + current + "; one network per container is supported"})
	}
	target := network
	if !connect {
		target = "bridge"
	}
	b.network(target)
	aliases := []string{}
	for k, v := range endpoint {
		if k == "Aliases" {
			aliases = stringsOf(v)
		} else if !idle(v) {
			panic(apiError{501, "Unsupported endpoint option: " + k})
		}
	}
	c["net_name"], c["ip"], c["aliases"] = target, b.assignIP(target), aliases
	save(path, c)
	b.refreshHosts(current)
	b.refreshHosts(target)
}
