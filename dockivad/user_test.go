package main

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func TestResolveUser(t *testing.T) {
	root := t.TempDir()
	check(os.MkdirAll(filepath.Join(root, "etc"), 0o755))
	check(os.WriteFile(filepath.Join(root, "etc", "passwd"), []byte("root:x:0:0:root:/root:/bin/sh\nredis:x:999:1000:Linux User,,,:/home/redis:/sbin/nologin\n"), 0o644))
	check(os.WriteFile(filepath.Join(root, "etc", "group"), []byte("root:x:0:\nredis:x:1000:redis\nstaff:x:50:\n"), 0o644))
	for user, want := range map[string]string{"": "0 0 /root", "redis": "999 1000 /home/redis", "redis:staff": "999 50 /home/redis", "999": "999 1000 /home/redis", "1234": "1234 0 /", "1234:5678": "1234 5678 /"} {
		uid, gid, home := resolveUser(root, user)
		if got := strings.Join([]string{itoa(uid), itoa(gid), home}, " "); got != want {
			t.Errorf("%q: %s, want %s", user, got, want)
		}
	}
	mustReject(t, func() { resolveUser(root, "nobody") })
	mustReject(t, func() { resolveUser(root, "redis:nogroup") })
	mustReject(t, func() { validUser("bad user") })
	if args := strings.Join(userArgs(root, obj{"user": "redis"}, []string{"PATH=/bin"}), " "); args != "--uid 999 --gid 1000 --env HOME=/home/redis" {
		t.Fatalf("args: %s", args)
	}
	if args := userArgs(root, obj{}, []string{"HOME=/data"}); len(args) != 0 {
		t.Fatalf("root with HOME set: %v", args)
	}
}
func itoa(n uint32) string { return strconv.FormatUint(uint64(n), 10) }
