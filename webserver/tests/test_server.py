"""정적 파일·HTTP·스레드 풀·과부하·종료 처리의 실제 TCP 통합 테스트."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from http.client import HTTPResponse
from pathlib import Path
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else "./webserver").resolve()
ADDRESS = ("127.0.0.1", 8080)
GET_CLOSE = b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"


def start_server(log, root, port=0):
    global ADDRESS
    process = subprocess.Popen(
        [str(BINARY), "--root", str(root), "--port", str(port)], stdout=log, stderr=log,
    )
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(log.read_text())
        match = re.search(r"서버 시작: 127\.0\.0\.1:(\d+)", log.read_text())
        if match:
            ADDRESS = ("127.0.0.1", int(match[1]))
            return process
        time.sleep(0.01)
    process.kill()
    process.wait()
    raise AssertionError("server startup timed out")


class ServerLog:
    def __init__(self, path):
        self.path = path
        self.file = path.open("wb")

    def fileno(self):
        return self.file.fileno()

    def read_text(self):
        return self.path.read_text(encoding="utf-8", errors="replace")


def exchange(parts):
    with socket.create_connection(ADDRESS, timeout=3) as client:
        for part in parts:
            client.sendall(part)
            if len(parts) > 1:
                time.sleep(0.01)
        client.shutdown(socket.SHUT_WR)
        return read_response(client)


def read_response(client):
    response = bytearray()
    while True:
        chunk = client.recv(65536)
        if not chunk:
            return bytes(response)
        response.extend(chunk)


def wait_for_count(log, text, count):
    deadline = time.monotonic() + 3
    while log.read_text().count(text) < count:
        assert time.monotonic() < deadline, (text, count, log.read_text())
        time.sleep(0.005)


def pool_limits(log):
    match = re.search(r"스레드 풀: 워커 (\d+)개, 작업 큐 (\d+)칸", log.read_text())
    assert match, log.read_text()
    return tuple(map(int, match.groups()))


def check_concurrent_requests(log, html, large):
    workers, _ = pool_limits(log)
    with ExitStack() as clients:
        idle = [clients.enter_context(socket.create_connection(ADDRESS, timeout=3))
                for _ in range(workers)]
        get_file("/", html, "text/html; charset=utf-8")

        # Split a GET across the handoff and verify the first byte reaches the worker.
        submitted = log.read_text().count("작업 큐 등록:")
        idle[0].sendall(GET_CLOSE[:1])
        wait_for_count(log, "작업 큐 등록:", submitted + 1)
        idle[0].sendall(GET_CLOSE[1:])
        response = read_response(idle[0])
        assert response.startswith(b"HTTP/1.1 200 OK\r\n"), response

        def request(number):
            if number % 3 == 0:
                get_file("/large.bin", large, "application/octet-stream")
            elif number % 3 == 1:
                get_file("/", html, "text/html; charset=utf-8")
            else:
                check_status(b"POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na\0b", 503)

        with ThreadPoolExecutor(max_workers=12) as concurrent:
            list(concurrent.map(request, range(36)))
    print(f"PASS: {workers} idle connections leave workers free, first byte preserved, 36 concurrent requests")


def check_pending_limit_and_shutdown(process, log, html):
    match = re.search(r"첫 요청 대기: 최대 (\d+)개", log.read_text())
    assert match, log.read_text()
    capacity = int(match[1])
    with ExitStack() as clients:
        waiting = [clients.enter_context(socket.create_connection(ADDRESS, timeout=3))
                   for _ in range(capacity)]
        with socket.create_connection(ADDRESS, timeout=3) as excess:
            assert excess.recv(1) == b"", "excess idle connection was not closed"
        waiting[0].sendall(GET_CLOSE)
        response = read_response(waiting[0])
        assert response.startswith(b"HTTP/1.1 200 OK\r\n"), response
        assert response.split(b"\r\n\r\n", 1)[1] == html, response
        get_file("/", html, "text/html; charset=utf-8")
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=3) == 0, log.read_text()
        for client in waiting[1:]:
            assert client.recv(1) == b"", "a first-byte waiter survived shutdown"
    print(f"PASS: {capacity} first-byte waiters, excess connection closed, slot reused, immediate SIGINT")


def check_full_queue_and_shutdown(process, log, html):
    workers, capacity = pool_limits(log)
    accepted_before = log.read_text().count("작업 큐 등록:")
    started_before = log.read_text().count("클라이언트 연결됨:")
    active = []
    queued = []
    try:
        for _ in range(workers):
            client = socket.create_connection(ADDRESS, timeout=3)
            active.append(client)
            # 본문용 메모리를 확보한 뒤 나머지 바이트를 기다리게 한다.
            client.sendall(b"POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\nx")
        wait_for_count(log, "클라이언트 연결됨:", started_before + workers)
        for index in range(capacity):
            client = socket.create_connection(ADDRESS, timeout=3)
            queued.append(client)
            client.sendall(GET_CLOSE if index == 0 else b"G")
        wait_for_count(log, "작업 큐 등록:", accepted_before + workers + capacity)

        queued[0].settimeout(0.1)
        try:
            queued[0].recv(1)
            raise AssertionError("all workers are busy, so this request must remain queued")
        except socket.timeout:
            pass
        queued[0].settimeout(3)

        before = time.monotonic()
        for _ in range(8):
            with socket.create_connection(ADDRESS, timeout=3) as extra:
                extra.sendall(GET_CLOSE)
                with HTTPResponse(extra) as response:
                    response.begin()
                    assert response.status == 503, response.status
                    assert response.getheader("Connection") == "close"
                    assert response.read() == b"503 Service Unavailable\n"
        assert time.monotonic() - before < 3, "overload handling blocked the accept thread"

        # 한 워커가 비면 큐에서 먼저 기다리던 GET을 처리한다.
        active[0].close()
        active[0] = None
        response = read_response(queued[0])
        assert response.startswith(b"HTTP/1.1 200 OK\r\n"), response
        assert response.split(b"\r\n\r\n", 1)[1] == html
        wait_for_count(log, "클라이언트 연결됨:", started_before + workers + 2)
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=3) == 0, log.read_text()
        assert "서버 종료" in log.read_text(), log.read_text()
        for client in active + queued[1:]:
            if client is not None:
                try:
                    assert client.recv(1) == b"", "a queued/active connection survived shutdown"
                except ConnectionResetError:
                    pass
        print(f"PASS: {workers} workers + {capacity} queued, prompt 503, FIFO recovery, immediate SIGINT with active/queued clients")
    finally:
        for client in active + queued:
            if client is not None:
                client.close()


def stop_while_sending(process, log):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as slow_reader:
        slow_reader.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        slow_reader.settimeout(3)
        slow_reader.connect(ADDRESS)
        slow_reader.sendall(b"GET /slow.bin HTTP/1.1\r\nHost: x\r\n\r\n")
        # 읽지 않는 상대에게 큰 응답을 보내는 도중 종료해도 send에서 30초 기다리지 않는다.
        assert slow_reader.recv(1, socket.MSG_PEEK) == b"H"
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=3) == 0, log.read_text()
        assert "서버 종료" in log.read_text(), log.read_text()
    print("PASS: immediate SIGTERM during a large response to a slow reader")


def check_status(wire, status):
    wire = wire.replace(b"\r\n", b"\r\nConnection: close\r\n", 1)
    response = exchange([wire])
    assert response.startswith(f"HTTP/1.1 {status} ".encode()), response
    headers, body = response.split(b"\r\n\r\n", 1)
    fields = dict(line.split(b": ", 1) for line in headers.split(b"\r\n")[1:])
    assert int(fields[b"Content-Length"]) == len(body), response
    assert fields[b"Connection"] == b"close", response
    return response


def get_file(target, expected, content_type):
    response = check_status(f"GET {target} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode(), 200)
    headers, body = response.split(b"\r\n\r\n", 1)
    assert body == expected, (target, len(body), len(expected))
    assert b"Content-Type: " + content_type.encode() + b"\r\n" in headers + b"\r\n", headers


def check_cli(root):
    help_result = subprocess.run([str(BINARY), "--help"], capture_output=True, timeout=3)
    assert help_result.returncode == 0
    assert b"--port" in help_result.stdout and b"--root" in help_result.stdout
    for arguments in (
        ["--port", "-1"], ["--port", "65536"], ["--port", "abc"], ["--port"],
        ["--root"], ["--root="], ["--unknown"],
        ["--root", str(root / "missing")], ["--root", str(root / "index.html")],
    ):
        result = subprocess.run([str(BINARY), *arguments], capture_output=True, timeout=3)
        assert result.returncode == 1, (arguments, result)


def stop_while_receiving(process, log, stop_signal):
    previous_connections = log.read_text().count("클라이언트 연결됨:")
    with socket.create_connection(ADDRESS, timeout=3) as idle:
        idle.sendall(b"GET / HTTP/1.1\r\n")
        wait_for_count(log, "클라이언트 연결됨:", previous_connections + 1)
        process.send_signal(stop_signal)
        assert process.wait(timeout=3) == 0, log.read_text()
        assert "서버 종료" in log.read_text(), log.read_text()


def run():
    with tempfile.TemporaryDirectory(prefix="week13-http-") as directory:
        root = Path(directory) / "www"
        root.mkdir()
        html = "<!doctype html><h1>정적 파일 응답</h1>".encode()
        (root / "index.html").write_bytes(html)
        (root / "style.css").write_bytes(b"body { margin: 0; }")
        (root / "app.js").write_bytes(b"console.log('ok');")
        png = b"\x89PNG\r\n\x1a\n\0binary"
        jpg = b"\xff\xd8\xff\0binary\xff\xd9"
        (root / "photo.PNG").write_bytes(png)
        (root / "photo.jpg").write_bytes(jpg)
        (root / "empty.txt").write_bytes(b"")
        (root / "hello world.txt").write_bytes(b"space")
        (root / "한글.txt").write_bytes("한글".encode())
        (root / "apix.txt").write_bytes(b"static")
        (root / "docs").mkdir()
        (root / "docs" / "index.html").write_bytes(b"nested index")
        (root / "private.txt").write_bytes(b"private")
        (root / "private.txt").chmod(0)
        secret = Path(directory) / "secret.txt"
        secret.write_bytes(b"must not be served")
        (root / "outside-link").symlink_to(secret)
        (root / "inside-link").symlink_to(root / "index.html")
        (root / "directory-link").symlink_to(root / "docs", target_is_directory=True)
        os.mkfifo(root / "pipe")
        large = bytes(range(256)) * (16 * 1024) + b"end"
        (root / "large.bin").write_bytes(large)
        with (root / "slow.bin").open("wb") as sparse:
            sparse.truncate(64 * 1024 * 1024)
        check_cli(root)
        log = ServerLog(Path(directory) / "server.log")
        process = None
        try:
            process = start_server(log, root)
            get_file("/", html, "text/html; charset=utf-8")
            get_file("/index.html?q=1", html, "text/html; charset=utf-8")
            get_file("/docs/", b"nested index", "text/html; charset=utf-8")
            get_file("/style.css", b"body { margin: 0; }", "text/css; charset=utf-8")
            get_file("/app.js", b"console.log('ok');", "text/javascript; charset=utf-8")
            get_file("/photo.PNG", png, "image/png")
            get_file("/photo.jpg", jpg, "image/jpeg")
            get_file("/empty.txt", b"", "text/plain; charset=utf-8")
            get_file("/hello%20world.txt", b"space", "text/plain; charset=utf-8")
            get_file("/%ED%95%9C%EA%B8%80.txt", "한글".encode(), "text/plain; charset=utf-8")
            get_file("/apix.txt", b"static", "text/plain; charset=utf-8")
            get_file("/large.bin", large, "application/octet-stream")

            for target in ("/missing", "/docs/missing", "/index.html/other"):
                check_status(f"GET {target} HTTP/1.1\r\nHost: x\r\n\r\n".encode(), 404)
            for target in (
                "/../secret.txt", "/%2e%2e/secret.txt", "/%2e%2e%2fsecret.txt",
                "/%5csecret.txt", "/outside-link", "/inside-link", "/directory-link/index.html",
                "/docs", "/pipe",
            ):
                check_status(f"GET {target} HTTP/1.1\r\nHost: x\r\n\r\n".encode(), 403)
            if os.geteuid() != 0:
                check_status(b"GET /private.txt HTTP/1.1\r\nHost: x\r\n\r\n", 403)
            for target in ("/bad%", "/bad%zz", "/a%00.txt", "/a%0d.txt"):
                check_status(f"GET {target} HTTP/1.1\r\nHost: x\r\n\r\n".encode(), 400)

            response = exchange([
                b"POST /api/echo HTTP/1.1\r\nHost: localhost\r\nContent-Len",
                b"gth: 11\r\n\r", b"\nhello", b" world",
            ])
            assert response.startswith(b"HTTP/1.1 503 "), response
            check_status(b"POST /api/binary HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na\0b", 503)
            check_status(b"POST /api/empty HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 503)
            check_status(b"GET /api HTTP/1.1\r\nHost: x\r\n\r\n", 503)
            response = check_status(b"POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 405)
            assert b"Allow: GET\r\n" in response
            check_status(b"POST / HTTP/1.1\r\nHost: x\r\n\r\n", 411)
            check_status(b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400)
            check_status(b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n", 413)
            check_status(b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\nhello", 400)
            check_status(b"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501)
            response = check_status(b"PUT / HTTP/1.1\r\nHost: x\r\n\r\n", 405)
            assert b"Allow: GET, POST\r\n" in response
            check_status(b"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505)
            check_status(b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nExpect: 100-continue\r\n\r\n", 417)

            # 상한 크기의 본문도 작은 수신 버퍼를 거쳐 전체 보관할 수 있어야 한다.
            body = b"x" * (1024 * 1024)
            header = f"POST /api/limit HTTP/1.1\r\nHost: x\r\nContent-Length: {len(body)}\r\n\r\n".encode()
            assert exchange([header + body]).startswith(b"HTTP/1.1 503 ")
            text = log.read_text()
            for expected in (
                "GET /index.html?q=1 HTTP/1.1, 본문 0바이트",
                "POST /api/echo HTTP/1.1, 본문 11바이트",
                "POST /api/binary HTTP/1.1, 본문 3바이트",
                "POST /api/empty HTTP/1.1, 본문 0바이트",
                "POST /api/limit HTTP/1.1, 본문 1048576바이트",
            ):
                assert expected in text, text

            # 응답 중 클라이언트가 끊겨도 다음 요청을 처리해야 한다.
            with socket.create_connection(ADDRESS, timeout=3) as aborted:
                aborted.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                aborted.sendall(b"GET /large.bin HTTP/1.1\r\nHost: x\r\n\r\n")
            get_file("/", html, "text/html; charset=utf-8")
            duplicate = subprocess.run(
                [str(BINARY), "--root", str(root), "--port", str(ADDRESS[1])],
                capture_output=True, timeout=3,
            )
            assert duplicate.returncode == 1, duplicate
            print("PASS: static HTML/CSS/JS/images, 4MiB binary transfer, path security, CLI, HTTP errors, POST bodies, disconnect")
            check_concurrent_requests(log, html, large)
            check_full_queue_and_shutdown(process, log, html)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            log.file.close()

        log = ServerLog(Path(directory) / "restart.log")
        process = None
        try:
            process = start_server(log, root, ADDRESS[1])
            stop_while_receiving(process, log, signal.SIGTERM)
            print("PASS: immediate restart and SIGTERM during an incomplete request")
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            log.file.close()
            (root / "private.txt").chmod(0o600)

        for name in ("sending", "idle"):
            log = ServerLog(Path(directory) / f"{name}.log")
            process = None
            try:
                process = start_server(log, root, ADDRESS[1])
                if name == "sending":
                    stop_while_sending(process, log)
                else:
                    check_pending_limit_and_shutdown(process, log, html)
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()
                log.file.close()


if __name__ == "__main__":
    run()
