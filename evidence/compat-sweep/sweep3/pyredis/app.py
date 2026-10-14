import socket, time
from http.server import BaseHTTPRequestHandler, HTTPServer
def incr():
    for _ in range(30):
        try:
            s = socket.create_connection(("redis", 6379), timeout=3); break
        except OSError: time.sleep(1)
    s.sendall(b"*2\r\n$4\r\nINCR\r\n$4\r\nhits\r\n"); r = s.recv(64); s.close()
    return int(r[1:].split(b"\r\n")[0])
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        body = f"hits {incr()}".encode(); self.send_response(200); self.end_headers(); self.wfile.write(body)
HTTPServer(("", 8000), H).serve_forever()
