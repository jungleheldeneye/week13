"""두 HTTP 백엔드와 실제 C 프록시 프로세스 사이의 TCP 통합 테스트."""

import base64
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import http.client
import json
from pathlib import Path
import re
import signal
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

from backend import Backend


BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else "./webserver").resolve()


@contextmanager
def backend(name):
    server = Backend(("127.0.0.1", 0), name)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        if server.stream_gate is not None:
            server.stream_gate.set()
        server.shutdown()
        server.server_close()
        thread.join(3)


class RawHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        wire = bytearray()
        try:
            while b"\r\n\r\n" not in wire:
                block = self.request.recv(4096)
                if not block:
                    return
                wire.extend(block)
            with self.server.lock:
                parts, hang = self.server.parts, self.server.hang
                self.server.received.append(bytes(wire))
            self.server.arrived.set()
            if hang:
                if not self.request.recv(1):
                    self.server.closed.set()
                return
            for part in parts:
                self.request.sendall(part)
        except (ConnectionError, TimeoutError):
            pass


class RawBackend(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self):
        self.lock = threading.Lock()
        self.parts = []
        self.hang = False
        self.arrived = threading.Event()
        self.closed = threading.Event()
        self.received = []
        super().__init__(("127.0.0.1", 0), RawHandler)


@contextmanager
def raw_backend():
    server = RawBackend()
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(3)


@contextmanager
def proxy(ports, timeout=1000, prefix="/api"):
    with tempfile.TemporaryDirectory(prefix="week13-proxy-") as directory:
        root = Path(directory)
        (root / "index.html").write_bytes(b"static stays local")
        path = root / "proxy.log"
        with path.open("wb") as log:
            command = [str(BINARY), "--port", "0", "--root", str(root),
                       "--proxy-timeout", str(timeout), "--proxy-prefix", prefix]
            for port in ports:
                command += ["--backend", f"127.0.0.1:{port}"]
            process = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while True:
                    text = path.read_text(errors="replace")
                    assert process.poll() is None, text
                    match = re.search(r"서버 시작: 127\.0\.0\.1:(\d+)", text)
                    if match:
                        break
                    assert time.monotonic() < deadline, text
                    time.sleep(0.005)
                yield int(match[1]), process, path
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    assert process.wait(3) == 0, path.read_text(errors="replace")
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()


@contextmanager
def backend_process(name):
    with tempfile.TemporaryDirectory(prefix="week13-backend-") as directory:
        path = Path(directory) / "backend.log"
        with path.open("wb") as log:
            process = subprocess.Popen([sys.executable, str(Path(__file__).with_name("backend.py")),
                                        "--port", "0", "--name", name], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while True:
                    text = path.read_text(errors="replace")
                    assert process.poll() is None, text
                    match = re.search(r"http://127\.0\.0\.1:(\d+)", text)
                    if match:
                        break
                    assert time.monotonic() < deadline, text
                    time.sleep(0.005)
                yield int(match[1])
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGINT)
                try:
                    assert process.wait(3) == 0, path.read_text(errors="replace")
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()


def request(port, target="/api/echo", method="GET", body=None, headers=None):
    client = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        client.request(method, target, body=body, headers={"Connection": "close", **(headers or {})})
        response = client.getresponse()
        return response.status, response.getheaders(), response.read()
    finally:
        client.close()


def wire_request(port, target="/api/echo", wire=None):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
        client.sendall(wire or f"GET {target} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode())
        response = bytearray()
        while True:
            block = client.recv(65536)
            if not block:
                return bytes(response)
            response.extend(block)


def check_normal(a, b):
    with proxy([a.server_port, b.server_port]) as (port, _, _):
        names = []
        for _ in range(6):
            status, headers, body = request(port, "/api/echo?q=a%20b")
            payload = json.loads(body)
            names.append(payload["backend"])
            assert status == 200 and payload["path"] == "/api/echo?q=a%20b", payload
            expected_port = a.server_port if payload["backend"] == "A" else b.server_port
            assert payload["host"] == f"127.0.0.1:{expected_port}", payload
            assert dict(headers)["Connection"] == "close"
            assert "X-Backend-Hop" not in dict(headers) and "Via" in dict(headers)
        assert names == ["A", "B"] * 3, names
        payload_bytes = b"\0binary\xff" + "한글".encode()
        status, _, body = request(port, method="POST", body=payload_bytes, headers={
            "Content-Type": "application/octet-stream", "Connection": "X-Remove, close",
            "X-Remove": "hidden", "X-Keep": "visible", "Proxy-Authorization": "hidden",
        })
        payload = json.loads(body)
        assert status == 200 and base64.b64decode(payload["body_base64"]) == payload_bytes
        assert payload["method"] == "POST" and payload["path"] == "/api/echo"
        assert "X-Remove" not in payload["headers"] and "Proxy-Authorization" not in payload["headers"]
        assert payload["headers"]["X-Keep"] == "visible"
        assert payload["headers"]["Connection"] == "close"
        assert request(port, "/")[2] == b"static stays local"
        # 정적 요청은 Round-Robin 순서를 소비하지 않는다.
        assert json.loads(request(port)[2])["backend"] == "B"
        with ThreadPoolExecutor(max_workers=12) as clients:
            names = list(clients.map(lambda _: json.loads(request(port)[2])["backend"], range(40)))
        assert Counter(names) == {"A": 20, "B": 20}, names
        big_body = b"x" * (1024 * 1024)
        assert base64.b64decode(json.loads(request(port, method="POST", body=big_body)[2])["body_base64"]) == big_body
        size = 4 * 1024 * 1024 + 3
        status, _, body = request(port, f"/api/bytes?size={size}")
        assert status == 200 and body == (bytes(range(256)) * ((size + 255) // 256))[:size]
    print("PASS: two-backend RR, concurrent RR, unchanged path/query, binary/1MiB POST, hop headers, 4MiB response, local static")


def check_streaming(server):
    server.stream_gate = threading.Event()
    try:
        with proxy([server.server_port]) as (port, _, _):
            client = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            try:
                client.request("GET", "/api/stream")
                response = client.getresponse()
                assert response.status == 200
                # 백엔드가 나머지를 보내기 전에 첫 부분이 클라이언트에 도착해야 한다.
                assert response.read(6) == b"first\n"
                assert not server.stream_gate.is_set()
                server.stream_gate.set()
                assert response.read() == b"second\n"
            finally:
                server.stream_gate.set()
                client.close()
    finally:
        server.stream_gate = None
    with proxy([server.server_port], prefix="/service") as (port, _, _):
        assert json.loads(request(port, "/service/echo")[2])["path"] == "/service/echo"
        assert request(port, "/api/echo")[0] == 404
    print("PASS: body streamed before backend finishes, configurable path prefix without rewriting")


def check_raw(server):
    cases = [
        ([b"HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA"], 200, b"hello"),
        ([b"HTTP/1.0 200 OK\r\n\r\nclose-delimited"], 200, b"close-delimited"),
        ([b"HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"], 200, b"ok"),
        ([b"HTTP/1.1 204 No Content\r\n\r\n"], 204, b""),
        ([b"HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n\r\n"], 304, b""),
        ([b"HTTP/1.1 418 Teapot\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nContent-Length: 2\r\n\r\nok"], 418, b"ok"),
        ([b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-End\r\n\r\n",
          b"3;test=yes\r\na\0b\r\n2\r\ncd\r\n0\r\nX-End: done\r\n\r\n"], 200, b"a\0bcd"),
        ([b"garbage\r\n\r\n"], 502, None),
        ([b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx"], 502, None),
        ([b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n"], 502, None),
        ([b"HTTP/1.1 101 Switching Protocols\r\n\r\n"], 502, None),
        ([b"HTTP/1.1 200 OK\r\nX: " + b"a" * 16384], 502, None),
        ([b"HTTP/1.1 200 OK\r\n"], 502, None),
        ([], 502, None),
    ]
    with proxy([server.server_address[1]]) as (port, _, _):
        for parts, expected, wanted in cases:
            with server.lock:
                server.parts = parts
            status, headers, body = request(port)
            assert status == expected, (parts, status, body)
            if wanted is not None:
                assert body == wanted, (parts, body, wanted)
            assert dict(headers).get("Connection") == "close"
            if expected == 418:
                assert [v for k, v in headers if k == "Set-Cookie"] == ["a=1", "b=2"]
            if any(b"Transfer-Encoding: chunked" in part for part in parts) and expected == 200:
                assert "Transfer-Encoding" not in dict(headers) and "Trailer" not in dict(headers)
        for wire in (
            b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort",
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n",
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nx",
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nBad Trailer\r\n\r\n",
        ):
            with server.lock:
                server.parts = [wire]
            result = wire_request(port)
            assert result.startswith(b"HTTP/1.1 200 ") and result.count(b"HTTP/1.1 ") == 1, result
        # 서로 다른 recv 경계에서 헤더·본문·chunk 구분자가 끊겨도 동작해야 한다.
        wire = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"
        with server.lock:
            server.parts = [bytes([byte]) for byte in wire]
        assert request(port)[2] == b"abc"
    print("PASS: length/EOF/chunked/interim/bodyless responses, fragmented framing, malformed/oversize headers, partial-body failures")


def check_failures(server, raw):
    with socket.socket() as refused:
        refused.bind(("127.0.0.1", 0))
        # C 서버가 시작한 뒤 예약 포트를 닫아, 실제로 리스너가 없는 포트에 연결한다.
        # bind만 한 소켓은 일부 OS에서 SYN을 거절하지 않고 무시해 타임아웃이 된다.
        with proxy([refused.getsockname()[1], server.server_port]) as (port, _, _):
            refused.close()
            failed = request(port)
            assert failed[0] == 502, failed
            assert request(port)[0] == 200
    with proxy([]) as (port, _, _):
        assert request(port)[0] == 503
    with proxy([server.server_port], timeout=100) as (port, _, _):
        assert request(port, "/api/delay?seconds=0.3")[0] == 504
        assert request(port)[0] == 200
    with raw.lock:
        raw.hang = True
    raw.arrived.clear()
    raw.closed.clear()
    with proxy([raw.server_address[1]], timeout=60000) as (port, process, log):
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            client.sendall(b"GET /api/echo HTTP/1.1\r\nHost: x\r\n\r\n")
            assert raw.arrived.wait(2)
            process.send_signal(signal.SIGTERM)
            assert process.wait(2) == 0, log.read_text(errors="replace")
            assert raw.closed.wait(2), "upstream socket survived proxy shutdown"
    with raw.lock:
        raw.hang = False
    print("PASS: refused backend 502, next RR request succeeds, no backend 503, idle 504, immediate shutdown of blocked upstream")


def main():
    with backend_process("CLI-A") as a_port, backend_process("CLI-B") as b_port:
        with proxy([a_port, b_port]) as (port, _, _):
            names = [json.loads(request(port)[2])["backend"] for _ in range(4)]
            assert names == ["CLI-A", "CLI-B"] * 2, names
    print("PASS: standalone backend CLI entrypoints and C proxy in three separate processes")
    with backend("A") as a, backend("B") as b, raw_backend() as raw:
        check_normal(a, b)
        check_streaming(a)
        check_raw(raw)
        check_failures(a, raw)


if __name__ == "__main__":
    main()
