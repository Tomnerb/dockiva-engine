package main

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"
	"unicode/utf8"
)

type obj = map[string]any
type apiError struct {
	Code    int
	Message string
}

func (e apiError) Error() string { return e.Message }
func reject(ok bool, message string) {
	if !ok {
		panic(apiError{400, message})
	}
}
func check(err error) {
	if err != nil {
		panic(err)
	}
}
func object(v any) obj {
	if v == nil {
		return obj{}
	}
	m, ok := v.(map[string]any)
	reject(ok, "Expected object")
	return m
}
func str(v any) string { s, ok := v.(string); reject(ok, "Expected string"); return s }
func text(v any) string {
	if v == nil {
		return ""
	}
	return str(v)
}
func integer(v any) int64 {
	n, ok := v.(json.Number)
	reject(ok, "Expected integer")
	x, e := n.Int64()
	reject(e == nil, "Expected bounded integer")
	return x
}
func array(v any) []any { a, ok := v.([]any); reject(ok, "Expected array"); return a }
func stringsOf(v any) []string {
	if v == nil {
		return []string{}
	}
	a := array(v)
	reject(len(a) <= 64, "Too many strings")
	out := []string{}
	for _, v := range a {
		s := str(v)
		reject(!strings.ContainsRune(s, 0), "NUL in string")
		out = append(out, s)
	}
	return out
}

// Reject duplicate object keys and excessive nesting before interpreting config.
func decode(data []byte, limit int64) obj {
	reject(int64(len(data)) <= limit, "JSON size limit exceeded")
	reject(utf8.Valid(data), "JSON must be UTF-8")
	d := json.NewDecoder(bytes.NewReader(data))
	d.UseNumber()
	var value func(int) any
	value = func(depth int) any {
		reject(depth <= 64, "JSON nesting limit exceeded")
		t, e := d.Token()
		if e != nil {
			panic(apiError{400, "Invalid JSON: " + e.Error()})
		}
		if delim, ok := t.(json.Delim); ok {
			switch delim {
			case '{':
				m := obj{}
				for d.More() {
					k, e := d.Token()
					if e != nil {
						panic(apiError{400, "Invalid JSON: " + e.Error()})
					}
					key := str(k)
					_, exists := m[key]
					reject(!exists, "Duplicate JSON key: "+key)
					m[key] = value(depth + 1)
				}
				_, e = d.Token()
				if e != nil {
					panic(apiError{400, "Invalid JSON: " + e.Error()})
				}
				return m
			case '[':
				a := []any{}
				for d.More() {
					a = append(a, value(depth+1))
				}
				_, e = d.Token()
				if e != nil {
					panic(apiError{400, "Invalid JSON: " + e.Error()})
				}
				return a
			default:
				reject(false, "Invalid JSON")
			}
		}
		return t
	}
	out, ok := value(0).(map[string]any)
	reject(ok, "Expected JSON object")
	_, e := d.Token()
	reject(e == io.EOF, "Trailing JSON data")
	return out
}
func digest(b []byte) string { x := sha256.Sum256(b); return "sha256:" + hex.EncodeToString(x[:]) }
func key(s string) string    { return digest([]byte(s))[7:] }
func token() string {
	b := make([]byte, 32)
	_, e := rand.Read(b)
	check(e)
	return hex.EncodeToString(b)
}
func now() float64 { return float64(time.Now().UnixNano()) / 1e9 }
func timestamp(v any) string {
	if v == nil {
		return "0001-01-01T00:00:00Z"
	}
	var n float64
	switch x := v.(type) {
	case json.Number:
		var e error
		n, e = x.Float64()
		check(e)
	case float64:
		n = x
	default:
		reject(false, "Invalid timestamp")
	}
	return time.Unix(0, int64(n*1e9)).UTC().Format(time.RFC3339Nano)
}
func private(path string) string {
	p, e := filepath.Abs(path)
	check(e)
	check(os.MkdirAll(p, 0700))
	info, e := os.Lstat(p)
	check(e)
	reject(info.IsDir() && info.Mode()&os.ModeSymlink == 0 && info.Mode().Perm()&0077 == 0, "State/cache must be a private directory (0700)")
	st := info.Sys().(*syscall.Stat_t)
	reject(st.Uid == uint32(os.Getuid()), "State/cache owner mismatch")
	return p
}
func noLinks(root, relative string) string {
	st, err := os.Lstat(root)
	check(err)
	reject(st.IsDir() && st.Mode()&os.ModeSymlink == 0, "Invalid directory root")
	p := root
	for _, part := range strings.Split(filepath.ToSlash(relative), "/") {
		reject(part != "..", "Path traversal rejected")
		if part == "" || part == "." {
			continue
		}
		p = filepath.Join(p, part)
		s, e := os.Lstat(p)
		check(e)
		reject(s.Mode()&os.ModeSymlink == 0, "Symlink rejected: "+p)
	}
	return p
}
func readBounded(root, relative string, limit int64) []byte {
	p := noLinks(root, relative)
	fd, e := syscall.Open(p, syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
	check(e)
	f := os.NewFile(uintptr(fd), p)
	defer f.Close()
	st, e := f.Stat()
	check(e)
	reject(st.Mode().IsRegular() && st.Size() <= limit, "Expected bounded regular file")
	b, e := io.ReadAll(io.LimitReader(f, limit+1))
	check(e)
	reject(int64(len(b)) <= limit, "File limit exceeded")
	return b
}
func readObject(path string) obj {
	return decode(readBounded(filepath.Dir(path), filepath.Base(path), 1<<20), 1<<20)
}
func save(path string, v any) {
	b, e := json.MarshalIndent(v, "", "  ")
	check(e)
	f, e := os.CreateTemp(filepath.Dir(path), ".state-")
	check(e)
	name := f.Name()
	defer os.Remove(name)
	defer f.Close()
	_, e = f.Write(append(b, '\n'))
	check(e)
	check(barrierSync(f))
	check(f.Close())
	check(os.Rename(name, path))
	dir, e := os.Open(filepath.Dir(path))
	check(e)
	defer dir.Close()
	check(barrierSync(dir))
}

// barrierSync orders a file's writes before everything written after it
// (F_BARRIERFSYNC): with the rename above, a crash or power loss leaves the old
// state or the new one, never a torn file. os.File.Sync is F_FULLFSYNC on macOS,
// which also empties the drive's cache and took milliseconds per state change.
func barrierSync(f *os.File) error {
	const fBarrierFsync = 85
	if _, _, errno := syscall.Syscall(syscall.SYS_FCNTL, f.Fd(), fBarrierFsync, 0); errno != 0 {
		return f.Sync() // a file system without barriers: a full sync
	}
	return nil
}
func lockFile(path string, nonblock bool) (*os.File, error) {
	fd, e := syscall.Open(path, syscall.O_CREAT|syscall.O_RDWR|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0600)
	if e != nil {
		return nil, e
	}
	f := os.NewFile(uintptr(fd), path)
	flags := syscall.LOCK_EX
	if nonblock {
		flags |= syscall.LOCK_NB
	}
	if e = syscall.Flock(fd, flags); e != nil {
		f.Close()
		return nil, e
	}
	return f, nil
}
func exists(path string) bool { _, e := os.Lstat(path); return e == nil }

// engineBinary is dir/name as the app bundle ships it, or dir/old where only the
// source build's name exists.
func engineBinary(dir, name, old string) string {
	if p := filepath.Join(dir, name); exists(p) {
		return p
	}
	return filepath.Join(dir, old)
}
func idle(v any) bool {
	if v == nil {
		return true
	}
	switch x := v.(type) {
	case bool:
		return !x
	case string:
		return x == ""
	case json.Number:
		return x == "0"
	case []any:
		return len(x) == 0
	case map[string]any:
		return len(x) == 0
	}
	return false
}
func regularExecutable(path string) string {
	p, e := filepath.Abs(path)
	check(e)
	s, e := os.Stat(p)
	check(e)
	reject(s.Mode().IsRegular() && s.Mode().Perm()&0111 != 0, "Runtime missing or not executable")
	return p
}
func safeRun(fn func()) (err error) {
	defer func() {
		if v := recover(); v != nil {
			if e, ok := v.(error); ok {
				err = e
			} else {
				err = fmt.Errorf("%v", v)
			}
		}
	}()
	fn()
	return
}
