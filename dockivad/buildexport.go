package main

// What a buildx build sends back through the client's session: the result's files
// (docker build --output type=local or type=tar, through FileSend.DiffCopy) and
// connections to the client's SSH agent (RUN --mount=type=ssh, SSH.ForwardAgent).

import (
	"archive/tar"
	"context"
	"errors"
	"io"
	"io/fs"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
)

// fsutil's packets (tonistiigi/fsutil types.Packet), as FileSync and FileSend carry them.
const exportChunk = 32 << 10

// exportFiles sends root's files to the client's FileSend service: as a directory
// tree (type=local: fsutil's sender side of what fetch receives) or as one tar
// stream (type=tar). exporter is the export's index in the solve request, which the
// client's session uses to pick its destination.
func (s *bkSession) exportFiles(ctx context.Context, root, kind string, exporter int) int64 {
	md := http.Header{}
	md.Set("buildkit-attachable-exporter-id", strconv.Itoa(exporter))
	c := grpcOpen(ctx, s.rt, "/moby.filesync.v1.FileSend/DiffCopy", md)
	defer c.close()
	if kind == "tar" {
		return exportTar(c, root)
	}
	return exportTree(c, root)
}

type bytesWriter struct{ c *grpcCall }

func (w bytesWriter) Write(p []byte) (int, error) {
	for done := 0; done < len(p); {
		n := min(len(p)-done, exportChunk)
		if e := w.c.send(pbuf{}.bytes(1, p[done:done+n])); e != nil { // BytesMessage
			return done, e
		}
		done += n
	}
	return len(p), nil
}

// exportTar streams root as a tar: regular files, directories and symbolic links.
func exportTar(c *grpcCall, root string) int64 {
	counted := &counter{}
	tw := tar.NewWriter(io.MultiWriter(bytesWriter{c}, counted))
	check(filepath.WalkDir(root, func(p string, d fs.DirEntry, e error) error {
		if e != nil || p == root {
			return e
		}
		info, e := d.Info()
		if e != nil {
			return e
		}
		rel, _ := filepath.Rel(root, p)
		return tarEntry(tw, filepath.ToSlash(rel), p, info)
	}))
	check(tw.Close())
	c.closeSend()
	for _, more := c.recv(); more; _, more = c.recv() {
	}
	return counted.n
}

// exportTree is fsutil's sender: a stat per entry in walk order (directories before
// what they hold), an empty stat, then each file the receiver asks for (by its
// stat's number) as data packets ending with an empty one, until it says FIN.
func exportTree(c *grpcCall, root string) int64 {
	var files []string
	listed := make(chan error, 1)
	go func() {
		listed <- safeRun(func() {
			check(filepath.WalkDir(root, func(p string, d fs.DirEntry, e error) error {
				if e != nil || p == root {
					return e
				}
				info, e := d.Info()
				if e != nil {
					return e
				}
				rel, _ := filepath.Rel(root, p)
				mode := info.Mode()
				st := pbuf{}.str(1, filepath.ToSlash(rel)).num(2, uint64(mode)).num(6, uint64(info.ModTime().UnixNano()))
				switch {
				case mode.IsRegular():
					st = st.num(5, uint64(info.Size()))
				case mode&fs.ModeSymlink != 0:
					target, e := os.Readlink(p)
					if e != nil {
						return e
					}
					st = st.str(7, target)
				case !mode.IsDir(): // devices, pipes and sockets stay behind
					return nil
				}
				files = append(files, p) // read by the receive loop only after the listing (listed)
				return c.send(pbuf{}.num(1, packetStat).msg(2, st))
			}))
			check(c.send(pbuf{}.num(1, packetStat))) // the end of the listing
		})
	}()
	var total int64
	listDone := false
	waitListing := func() {
		if !listDone {
			check(<-listed)
			listDone = true
		}
	}
	for {
		p, ok := c.recv()
		reject(ok, "The client ended the export early")
		switch p.num(1) {
		case packetErr:
			panic(apiError{400, "The client could not write the export: " + p.str(4)})
		case packetFin:
			waitListing()
			check(c.send(pbuf{}.num(1, packetFin)))
			c.closeSend()
			for _, more := c.recv(); more; _, more = c.recv() {
			}
			return total
		case packetReq:
			waitListing() // the files' numbers are only final once listed
			id := p.num(3)
			reject(id < uint64(len(files)), "The client asked for an unknown file")
			total += sendFile(c, uint32(id), files[id])
		}
	}
}

func sendFile(c *grpcCall, id uint32, p string) int64 {
	f, e := os.Open(p)
	check(e)
	defer f.Close()
	buf := make([]byte, exportChunk)
	var n int64
	for {
		k, e := f.Read(buf)
		if k > 0 {
			check(c.send(pbuf{}.num(1, packetData).num(3, uint64(id)).bytes(4, buf[:k])))
			n += int64(k)
		}
		if errors.Is(e, io.EOF) {
			break
		}
		check(e)
	}
	check(c.send(pbuf{}.num(1, packetData).num(3, uint64(id)))) // the end of the file
	return n
}

// sshAgent checks that the client forwards the SSH agent id (docker build --ssh id)
// and returns what connects a step's socket connection to it.
func (s *bkSession) sshAgent(ctx context.Context, id string) (func(net.Conn), error) {
	if safeRun(func() { grpcUnary(ctx, s.rt, "/moby.sshforward.v1.SSH/CheckAgent", nil, pbuf{}.str(1, id)) }) != nil {
		return nil, errors.New("the build has no SSH agent " + strconv.Quote(id) + "; forward one with docker build --ssh " + id)
	}
	return func(conn net.Conn) {
		defer conn.Close()
		md := http.Header{}
		md.Set("ssh-id", id)
		c := grpcOpen(ctx, s.rt, "/moby.sshforward.v1.SSH/ForwardAgent", md)
		defer c.close()
		go func() {
			_, _ = io.Copy(bytesWriter{c}, conn)
			c.closeSend()
		}()
		_ = safeRun(func() {
			for {
				m, more := c.recv()
				if !more {
					return
				}
				if _, e := conn.Write(m.last(1).b); e != nil {
					return
				}
			}
		})
	}, nil
}
