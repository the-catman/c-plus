#!/usr/bin/env python3
"""A local server for the tests that `import http`, so that they need no network.

It prints the URLs the tests use as one line of JSON, then serves until it is killed. tests/run_tests.py gives them
to programs as environment variables:

  CPLUS_TEST_HTTP        http://127.0.0.1:port
  CPLUS_TEST_HTTP_OTHER  the same server, as another origin to a client: http://localhost:port
  CPLUS_TEST_HTTPS       https://localhost:port, with a certificate of its own
  CPLUS_TEST_HTTPS_IP    the same, by a name the certificate does not have: https://127.0.0.1:port
  CPLUS_TEST_CA          the file with that certificate, for caFile
  CPLUS_TEST_WS          ws://127.0.0.1:port
  CPLUS_TEST_WSS         wss://localhost:port
  CPLUS_TEST_HTTP6       http://[::1]:port, if this machine has IPv6
  CPLUS_TEST_WS6         ws://[::1]:port, if this machine has IPv6
"""
import base64
import gzip
import hashlib
import http.server
import json
import pathlib
import random
import select
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

BOMB = gzip.compress(b"\0" * (32 * 1024 * 1024))
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
connections = iter(range(1, 1 << 40))
tunnels = 0


def frame(opcode, payload=b"", fin=True, rsv=0, masked=False, length_bytes=None):
    """A frame as a server would send it, or as a broken one would, by the choice of the arguments."""
    head = bytes([(0x80 if fin else 0) | rsv | opcode])
    n = len(payload)
    mask = 0x80 if masked else 0
    width = length_bytes if length_bytes is not None else (0 if n < 126 else 2 if n < 65536 else 8)
    if width == 0:
        head += bytes([mask | n])
    elif width == 2:
        head += bytes([mask | 126]) + struct.pack(">H", n)
    else:
        head += bytes([mask | 127]) + struct.pack(">Q", n)
    if masked:
        head += b"\0\0\0\0"  # a key of zeros leaves the payload as it is
    return head + payload


class Peer:
    """The server's side of a WebSocket."""

    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""

    def read(self, n):
        while len(self.buffer) < n:
            got = self.sock.recv(65536)
            if not got:
                raise ConnectionError
            self.buffer += got
        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def next_frame(self):
        first, second = self.read(2)
        if not second & 0x80:
            raise ValueError("a frame from a client was not masked")
        n = second & 0x7F
        if n == 126:
            n = struct.unpack(">H", self.read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self.read(8))[0]
        key = self.read(4)
        self.last_key = key
        data = self.read(n)
        return first & 0x0F, bytes(b ^ key[i & 3] for i, b in enumerate(data)) if n < 1 << 20 else unmask_fast(data, key)

    def send(self, data):
        self.sock.sendall(data)

    def close_with(self, code, reason=b""):
        self.send(frame(8, struct.pack(">H", code) + reason))


def unmask_fast(data, key):
    n = len(data)
    whole = int.from_bytes(key * (n // 4 + 1), "little") & ((1 << (8 * n)) - 1)
    return (int.from_bytes(data, "little") ^ whole).to_bytes(n, "little")


def echo(peer):
    while True:
        opcode, data = peer.next_frame()
        if opcode == 8:
            peer.send(frame(8, data))
            return
        if opcode in (1, 2):
            peer.send(frame(opcode, data))


def closes_after_hello(peer):
    peer.send(frame(1, b"hello"))
    peer.close_with(4001, b"all done")
    peer.next_frame()


def fragments(peer):
    """A message in three pieces, with a ping between them, which the client must answer with the same bytes."""
    peer.send(frame(1, b"one ", fin=False))
    peer.send(frame(9, b"are you there"))
    peer.send(frame(0, b"two ", fin=False))
    opcode, data = peer.next_frame()
    pong = opcode == 10 and data == b"are you there"
    peer.send(frame(0, b"three"))
    peer.send(frame(1, b"pong was right" if pong else b"pong was wrong"))
    peer.close_with(1000)


def one_byte_at_a_time(peer):
    peer.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    for b in frame(1, b"slowly") + frame(1, b"x" * 300):
        peer.send(bytes([b]))
        time.sleep(0.0005)
    peer.close_with(1000)


def drop(peer):
    peer.send(frame(1, b"before the drop"))


def later(peer):
    """Half a frame, a wait, and then the rest: a timeout in between must lose nothing."""
    whole = frame(1, b"worth the wait")
    peer.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    peer.send(whole[:5])
    time.sleep(0.4)
    peer.send(whole[5:])
    echo(peer)


def hang(peer):
    time.sleep(3)


def ignores_close(peer):
    while peer.sock.recv(65536):
        pass


def mask_keys(peer):
    """What a client sends must be masked with a key that cannot be guessed, so at least they must differ."""
    keys = []
    for _ in range(8):
        peer.next_frame()
        keys.append(peer.last_key)
    peer.send(frame(1, b"all different" if len(set(keys)) == len(keys) else b"some were the same"))
    peer.close_with(1000)


def binary_all_bytes(peer):
    peer.send(frame(2, bytes(range(256))))
    peer.close_with(1000)


def many_small(peer):
    peer.send(b"".join(frame(1, str(i).encode()) for i in range(2000)))
    peer.close_with(1000)


def large_message(peer):
    n = int(peer.query.get("bytes", ["0"])[0])
    peer.send(frame(2, b"z" * n))
    peer.close_with(1000)


def garbage(peer):
    """What a broken or hostile server might send: a few valid frames, then bytes at random."""
    rng = random.Random(int(peer.query.get("seed", ["0"])[0]))
    out = b""
    for _ in range(rng.randrange(4)):
        out += frame(rng.choice([1, 2, 9, 10]), bytes(rng.randrange(128) for _ in range(rng.randrange(40))))
    out += bytes(rng.randrange(256) for _ in range(rng.randrange(400)))
    peer.send(out)


def show_request(peer):
    peer.send(frame(1, json.dumps(peer.request).encode()))
    peer.close_with(1000)


def broken(data):
    def script(peer):
        peer.send(data)
        try:
            opcode, payload = peer.next_frame()  # the close that says why
            peer.request["close"] = struct.unpack(">H", payload[:2])[0] if opcode == 8 and len(payload) >= 2 else 0
        except (ConnectionError, ValueError, OSError):
            pass
    return script


WEBSOCKETS = {
    "echo": echo,
    "closes": closes_after_hello,
    "fragments": fragments,
    "slowly": one_byte_at_a_time,
    "drop": drop,
    "later": later,
    "hang": hang,
    "ignores-close": ignores_close,
    "binary": binary_all_bytes,
    "mask-keys": mask_keys,
    "many": many_small,
    "large": large_message,
    "garbage": garbage,
    "show-request": show_request,
    "masked": broken(frame(1, b"masked", masked=True)),
    "reserved": broken(frame(1, b"reserved", rsv=0x40)),
    "unknown-opcode": broken(frame(3, b"what")),
    "split-ping": broken(frame(9, b"ping", fin=False)),
    "long-ping": broken(frame(9, b"p" * 126)),
    "long-length-16": broken(frame(1, b"short", length_bytes=2)),
    "long-length-64": broken(frame(1, b"short", length_bytes=8)),
    "impossible-length": broken(b"\x81\x7f\xff\xff\xff\xff\xff\xff\xff\xff"),
    "stray-continuation": broken(frame(0, b"where from")),
    "second-start": broken(frame(1, b"one", fin=False) + frame(1, b"two")),
    "bad-text": broken(frame(1, b"\xff\xfe")),
    "bad-text-in-pieces": broken(frame(1, b"\xe2\x82", fin=False) + frame(0, b"\xff")),
    "short-close": broken(frame(8, b"\x03")),
    "bad-close-code": broken(frame(8, struct.pack(">H", 1005))),
    "bad-close-text": broken(frame(8, struct.pack(">H", 1000) + b"\xff")),
    "huge": broken(b"\x82\x7f\x00\x00\x00\x00\x7f\xff\xff\xff"),
    "pieces-over": broken(frame(2, b"p" * 600, fin=False) + frame(0, b"p" * 600)),
}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "cplus-test"
    disable_nagle_algorithm = True  # its headers and body go separately, and would each wait for the other side's reply

    def setup(self):
        super().setup()
        self.connection_id = next(connections)

    def log_message(self, *args):
        pass

    def reply(self, status=200, body=b"", headers=()):
        self.send_response(status)
        names = {name.lower() for name, _ in headers}
        for name, value in headers:
            self.send_header(name, value)
        if "content-length" not in names and "transfer-encoding" not in names:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def read_body(self):
        if "chunked" in self.headers.get("Transfer-Encoding", ""):
            raise ValueError("no chunked bodies are expected")
        return self.rfile.read(int(self.headers.get("Content-Length", "0")))

    def do_CONNECT(self):
        """Enough of a proxy to carry a connection, and count it."""
        global tunnels
        tunnels += 1
        host, port = self.path.rsplit(":", 1)
        far = socket.create_connection((host, int(port)))
        self.send_response(200)
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True
        here = self.connection
        try:
            while True:
                ready, _, _ = select.select([here, far], [], [], 5)
                if not ready:
                    return
                for sock in ready:
                    data = sock.recv(65536)
                    if not data:
                        return
                    (far if sock is here else here).sendall(data)
        finally:
            far.close()

    def route(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        path = url.path
        parts = path.strip("/").split("/")
        body = self.read_body()
        if parts[0] == "ws":
            return self.websocket(parts[1] if len(parts) > 1 else "", query)
        if path == "/hello":
            return self.reply(200, b"hello", [("Content-Type", "text/plain"), ("X-Mixed-Case", "yes")])
        if path == "/echo":
            answer = {"method": self.command, "path": self.path, "body": body.decode("latin-1"),
                      "headers": {k.lower(): v for k, v in self.headers.items()}}
            return self.reply(200, json.dumps(answer).encode(), [("Content-Type", "application/json")])
        if parts[0] == "status":
            return self.reply(int(parts[1]), b"status " + parts[1].encode())
        if parts[0] == "redirect":
            target = query["to"][0]
            return self.reply(int(parts[1]), b"go there", [("Location", target)])
        if path == "/no-location":
            return self.reply(302, b"no place to go")
        if path == "/loop":
            return self.reply(302, b"", [("Location", "/loop")])
        if parts[0] == "chain":
            n = int(parts[1])
            return self.reply(302, b"", [("Location", f"/chain/{n - 1}")]) if n > 0 else self.reply(200, b"the end")
        if path == "/slow":
            time.sleep(float(query["seconds"][0]))
            return self.reply(200, b"finally")
        if path == "/gzip":
            return self.reply(200, gzip.compress(b"small once decoded"), [("Content-Encoding", "gzip")])
        if path == "/bad-gzip":
            return self.reply(200, b"this is not gzip at all", [("Content-Encoding", "gzip")])
        if path == "/bomb":
            return self.reply(200, BOMB, [("Content-Encoding", "gzip")])
        if path == "/big":
            return self.reply(200, b"b" * int(query["bytes"][0]))
        if path == "/binary":
            return self.reply(200, bytes(range(256)), [("Content-Type", "application/octet-stream")])
        if path == "/empty":
            return self.reply(204)
        if path == "/cookies":
            return self.reply(200, b"", [("Set-Cookie", "a=1"), ("X-Other", "between"), ("Set-Cookie", "b=2")])
        if path == "/many-values":
            return self.reply(200, b"", [("X-Tag", "one"), ("x-tag", "two")])
        if path == "/folded":
            self.send_response(200)
            self._headers_buffer.append(b"X-Folded: first\r\n   second\r\n")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        if path == "/odd-headers":
            self.send_response(200)
            self._headers_buffer.append(b"X-Good: 1\r\nX-Spaces:    spaced out   \r\nX-Empty:\r\nX-Colons: a:b:c\r\n")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        if path == "/early-hints":
            self.wfile.write(b"HTTP/1.1 103 Early Hints\r\nX-Early: yes\r\n\r\n")
            self.wfile.flush()
            return self.reply(200, b"after the hints", [("X-Final", "yes")])
        if path == "/no-colon":
            self.send_response(200)
            self._headers_buffer.append(b"no colon on this line\r\n")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        if path == "/no-name":
            self.send_response(200)
            self._headers_buffer.append(b": no name\r\nX-After: yes\r\n")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        if path == "/chunked":
            self.reply(200, b"", [("Transfer-Encoding", "chunked")])
            for piece in (b"in ", b"three ", b"pieces"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")
            return None
        if path == "/truncated":
            self.close_connection = True
            return self.reply(200, b"only this", [("Content-Length", "100")])
        if path == "/not-http":
            self.close_connection = True
            self.wfile.write(b"this is not HTTP\r\n\r\n")
            return None
        if path == "/connection":
            return self.reply(200, str(self.connection_id).encode())
        if path == "/connection-closes":
            self.close_connection = True
            return self.reply(200, str(self.connection_id).encode(), [("Connection", "close")])
        if path == "/tunnels":
            return self.reply(200, str(tunnels).encode())
        return self.reply(404, b"no such page")

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = do_OPTIONS = do_BREW = route

    def websocket(self, script, query):
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        lines = ["Upgrade: websocket", "Connection: Upgrade", f"Sec-WebSocket-Accept: {accept}"]
        self.close_connection = True
        if script == "not-found":
            return self.reply(404, b"no socket here")
        if script == "no-upgrade":
            lines = [f"Sec-WebSocket-Accept: {accept}"]
        elif script == "wrong-accept":
            lines[2] = "Sec-WebSocket-Accept: bm90IHRoZSByaWdodCBvbmU="
        elif script == "no-accept":
            lines = lines[:2]
        elif script == "split-headers":
            lines = ["Upgrade: h2c", "Upgrade: websocket", "Connection: keep-alive", "Connection: Upgrade", f"Sec-WebSocket-Accept: {accept}"]
        elif script == "extension":
            lines.append("Sec-WebSocket-Extensions: permessage-deflate")
        elif script == "protocol":
            lines.append("Sec-WebSocket-Protocol: chat")
        elif script == "no-answer":
            return None
        elif script == "silent":
            time.sleep(3)
            return None
        elif script == "not-http":
            self.wfile.write(b"this is not HTTP\r\n\r\n")
            return None
        elif script == "endless-handshake":
            self.wfile.write(b"HTTP/1.1 101 Switching Protocols\r\n" + b"X-Long: " + b"x" * 200000)
            return None
        first_frame = frame(1, b"came with the handshake") if script == "leftover" else b""
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\n" + "\r\n".join(lines) + "\r\n\r\n").encode() + first_frame)
        self.wfile.flush()
        peer = Peer(self.connection)
        peer.query = query
        peer.request = {"path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}}
        try:
            WEBSOCKETS.get(script, echo)(peer)
        except (ConnectionError, ValueError, OSError):
            pass
        return None


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128

    def handle_error(self, request, client_address):
        pass  # clients that go away, and tests that do it on purpose


class ServerOnIpv6(Server):
    address_family = socket.AF_INET6


class TlsServer(Server):
    def __init__(self, address, handler, context):
        super().__init__(address, handler)
        self.context = context

    def get_request(self):
        sock, address = super().get_request()
        return self.context.wrap_socket(sock, server_side=True, do_handshake_on_connect=False), address


def make_certificate(folder):
    cert = folder / "cert.pem"
    key = folder / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "2",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"], check=True, capture_output=True)
    return cert, key


def main():
    with tempfile.TemporaryDirectory(prefix="cplus-http-") as tmp:
        folder = pathlib.Path(tmp)
        cert, key = make_certificate(folder)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        plain = Server(("127.0.0.1", 0), Handler)
        secure = TlsServer(("127.0.0.1", 0), Handler, context)
        for server in (plain, secure):
            threading.Thread(target=server.serve_forever, daemon=True).start()
        p, s = plain.server_address[1], secure.server_address[1]
        addresses = {}
        try:  # only what a machine with an IPv6 address has
            on_ipv6 = ServerOnIpv6(("::1", 0), Handler)
            threading.Thread(target=on_ipv6.serve_forever, daemon=True).start()
            addresses["CPLUS_TEST_HTTP6"] = f"http://[::1]:{on_ipv6.server_address[1]}"
            addresses["CPLUS_TEST_WS6"] = f"ws://[::1]:{on_ipv6.server_address[1]}"
        except OSError:
            pass
        print(json.dumps({
            **addresses,
            "CPLUS_TEST_HTTP": f"http://127.0.0.1:{p}",
            "CPLUS_TEST_HTTP_OTHER": f"http://localhost:{p}",
            "CPLUS_TEST_HTTPS": f"https://localhost:{s}",
            "CPLUS_TEST_HTTPS_IP": f"https://127.0.0.1:{s}",
            "CPLUS_TEST_CA": str(cert),
            "CPLUS_TEST_WS": f"ws://127.0.0.1:{p}",
            "CPLUS_TEST_WSS": f"wss://localhost:{s}",
        }), flush=True)
        try:
            sys.stdin.read()  # until the runner goes away, however it does
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
