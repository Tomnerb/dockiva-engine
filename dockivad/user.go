package main

import (
	"bufio"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

var userPattern = regexp.MustCompile(`^[a-zA-Z0-9_.-]{1,32}(:[a-zA-Z0-9_.-]{1,32})?$`)

// validUser checks a Docker USER (name, uid, name:group, uid:gid ...); "" is root.
func validUser(u string) string {
	reject(u == "" || userPattern.MatchString(u), "Invalid user: "+u)
	return u
}

// passwdEntries reads name -> fields from a container's /etc/passwd or /etc/group
// (a missing file has no entries). Lines are name:x:id:...
func passwdEntries(root, file string) map[string][]string {
	out := map[string][]string{}
	var data []byte
	if safeRun(func() { data = readBounded(root, file, 1<<20) }) != nil {
		return out
	}
	scanner := bufio.NewScanner(strings.NewReader(string(data)))
	for scanner.Scan() {
		fields := strings.Split(scanner.Text(), ":")
		if len(fields) >= 3 && fields[0] != "" {
			out[fields[0]] = fields
		}
	}
	return out
}

// resolveUser turns a USER into the uid, gid and home directory a process starts
// with, from the container's own /etc/passwd and /etc/group, as Docker does. A
// numeric uid not in passwd gets gid 0 and home "/".
func resolveUser(root, user string) (uid, gid uint32, home string) {
	if user == "" {
		user = "0"
	}
	name, group, hasGroup := strings.Cut(user, ":")
	passwd := passwdEntries(root, filepath.Join("etc", "passwd"))
	number := func(s string) (uint32, bool) {
		n, e := strconv.ParseUint(s, 10, 32)
		return uint32(n), e == nil && n < 1<<31
	}
	home = "/"
	if n, ok := number(name); ok {
		uid = n
		for _, f := range passwd {
			if id, ok := number(f[2]); ok && id == n && len(f) >= 4 {
				g, _ := number(f[3])
				gid = g
				if len(f) >= 6 && f[5] != "" {
					home = f[5]
				}
				break
			}
		}
	} else {
		f, ok := passwd[name]
		reject(ok && len(f) >= 4, "Unable to find user "+name+" in the container's /etc/passwd")
		u, ok1 := number(f[2])
		g, ok2 := number(f[3])
		reject(ok1 && ok2, "Invalid /etc/passwd entry for "+name)
		uid, gid = u, g
		if len(f) >= 6 && f[5] != "" {
			home = f[5]
		}
	}
	if hasGroup {
		if n, ok := number(group); ok {
			gid = n
		} else {
			f, ok := passwdEntries(root, filepath.Join("etc", "group"))[group]
			reject(ok, "Unable to find group "+group+" in the container's /etc/group")
			g, ok := number(f[2])
			reject(ok, "Invalid /etc/group entry for "+group)
			gid = g
		}
	}
	return uid, gid, home
}

// userArgs, in the supervisor: --uid/--gid for a non-root USER, and HOME from
// passwd when the environment does not set it.
func userArgs(root string, config obj, env []string) []string {
	uid, gid, home := resolveUser(root, text(config["user"]))
	args := []string{}
	if uid != 0 || gid != 0 {
		args = append(args, "--uid", strconv.FormatUint(uint64(uid), 10), "--gid", strconv.FormatUint(uint64(gid), 10))
	}
	for _, v := range env {
		if strings.HasPrefix(v, "HOME=") {
			return args
		}
	}
	return append(args, "--env", "HOME="+home)
}
