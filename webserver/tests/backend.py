"""실행 예: python3 tests/backend.py --port 8001 --name A

과제의 C 서버와 별도로 실행하는 HTTP 테스트 백엔드. 외부 패키지는 필요 없다.
"""

import argparse
import base64
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


class Backend(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, name):
        self.name = name
        self.stream_gate = None
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 5

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.handle_api()

    def do_POST(self):
        self.handle_api()

    def handle_api(self):
        self.close_connection = True
        target = urlsplit(self.path)
        parameters = parse_qs(target.query)
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length < 0 or length > 1024 * 1024:
                self.send_error(413)
                return
            body = self.rfile.read(length)
            if len(body) != length:
                return
            if target.path.endswith("/delay"):
                time.sleep(min(float(parameters.get("seconds", ["1"])[0]), 5))
            if target.path.endswith("/stream"):
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", "13")
                self.end_headers()
                self.wfile.write(b"first\n")
                self.wfile.flush()
                if self.server.stream_gate is not None:
                    self.server.stream_gate.wait(3)
                else:
                    time.sleep(0.5)
                self.wfile.write(b"second\n")
                return
            if target.path.endswith("/bytes"):
                size = max(0, min(int(parameters.get("size", ["65536"])[0]), 64 * 1024 * 1024))
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.end_headers()
                block = bytes(range(256)) * 64
                while size:
                    chunk = block[:min(size, len(block))]
                    self.wfile.write(chunk)
                    size -= len(chunk)
                return
            payload = {
                "backend": self.server.name,
                "method": self.command,
                "path": self.path,
                "host": self.headers.get("Host"),
                "headers": dict(self.headers.items()),
                "body": body.decode("utf-8", errors="replace"),
                "body_base64": base64.b64encode(body).decode("ascii"),
            }
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close, X-Backend-Hop")
            self.send_header("X-Backend-Hop", "remove-me")
            self.send_header("X-Backend", self.server.name)
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        except ValueError:
            self.send_error(400)


def main():
    parser = argparse.ArgumentParser(description="C Reverse Proxy용 HTTP 테스트 백엔드")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--name", default="A")
    options = parser.parse_args()
    with Backend(("127.0.0.1", options.port), options.name) as server:
        print(f"backend {options.name}: http://127.0.0.1:{server.server_port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
