"""실제 TCP 연결 재사용·요청 경계·5초 유휴 회수·종료 검증."""

import base64
import http.client
import json
import signal
import socket
import time

from test_proxy_server import backend, proxy, raw_backend


def read_frame(stream):
    line = stream.readline()
    assert line.startswith(b"HTTP/1.1 "), line
    status = int(line.split()[1])
    headers = {}
    while True:
        line = stream.readline()
        assert line, "EOF inside response headers"
        if line == b"\r\n":
            break
        name, value = line.rstrip(b"\r\n").split(b":", 1)
        headers[name.lower()] = value.strip()
    length = 0 if status in (204, 304) else int(headers[b"content-length"])
    body = stream.read(length)
    assert len(body) == length, (length, body)
    return status, headers, body


def get(target="/", version="HTTP/1.1", connection=None):
    header = f"GET {target} {version}\r\nHost: x\r\n"
    if connection:
        header += f"Connection: {connection}\r\n"
    return (header + "\r\n").encode()


def check_reuse(a, b):
    with proxy([a.server_port, b.server_port]) as (port, _, _):
        client = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        try:
            original = None
            for number in range(20):
                body = bytes([number, 0, 255])
                client.request("POST", "/api/echo", body=body)
                reply = client.getresponse()
                payload = json.loads(reply.read())
                assert reply.getheader("Connection") == "keep-alive"
                assert payload["backend"] == ("A" if number % 2 == 0 else "B")
                assert base64.b64decode(payload["body_base64"]) == body, payload
                if original is None:
                    original = client.sock
                assert client.sock is original, "client silently opened a new TCP connection"
                client.request("GET", "/")
                reply = client.getresponse()
                assert reply.read() == b"static stays local"
                assert client.sock is original
            client.request("GET", "/", headers={"Connection": "close"})
            reply = client.getresponse()
            assert reply.getheader("Connection") == "close"
            assert reply.read() == b"static stays local" and client.sock is None
        finally:
            client.close()
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            with client.makefile("rb") as stream:
                client.sendall(b"POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na\0b"
                               + get(connection="close"))
                payload = json.loads(read_frame(stream)[2])
                assert base64.b64decode(payload["body_base64"]) == b"a\0b"
                assert read_frame(stream)[2] == b"static stays local"
                assert stream.read(1) == b""
    print("PASS: one TCP socket reused for 40 static/POST requests, request cleanup, per-request RR, explicit close")


def check_pipeline_and_versions():
    with proxy([]) as (port, _, _):
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            with client.makefile("rb") as stream:
                client.sendall(get() + get() + get(connection="close") + get())
                for number in range(3):
                    status, headers, body = read_frame(stream)
                    assert status == 200 and body == b"static stays local"
                    assert headers[b"connection"] == (b"close" if number == 2 else b"keep-alive")
                assert stream.read(1) == b"", "request after Connection: close was processed"
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            with client.makefile("rb") as stream:
                # POST 본문 뒤에 붙은 다음 요청을 본문으로 소비하면 안 된다.
                client.sendall(get() + b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na\0b" + get())
                assert read_frame(stream)[0] == 200
                status, headers, _ = read_frame(stream)
                assert status == 405 and headers[b"connection"] == b"close"
                assert stream.read(1) == b""
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            with client.makefile("rb") as stream:
                client.sendall(get(version="HTTP/1.0", connection="keep-alive"))
                assert read_frame(stream)[1][b"connection"] == b"keep-alive"
                client.sendall(get(version="HTTP/1.0"))
                assert read_frame(stream)[1][b"connection"] == b"close"
                assert stream.read(1) == b""
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            with client.makefile("rb") as stream:
                client.sendall(get())
                assert read_frame(stream)[0] == 200
                client.sendall(b"GET / HTTP/1.1\r\n\r\n")
                status, headers, _ = read_frame(stream)
                assert status == 400 and headers[b"connection"] == b"close"
                assert stream.read(1) == b""
    print("PASS: pipelined boundaries/order, close stops buffered requests, HTTP/1.0 opt-in, malformed next request closes")


def check_idle_recovery_and_shutdown():
    with proxy([]) as (port, process, log):
        clients = []
        streams = []
        try:
            for _ in range(4):
                client = socket.create_connection(("127.0.0.1", port), timeout=8)
                clients.append(client)
                streams.append(client.makefile("rb"))
                client.sendall(get())
                assert read_frame(streams[-1])[0] == 200
            started = time.monotonic()
            with socket.create_connection(("127.0.0.1", port), timeout=8) as queued:
                queued.sendall(get(connection="close"))
                with queued.makefile("rb") as stream:
                    assert read_frame(stream)[0] == 200
                elapsed = time.monotonic() - started
                assert 3.5 < elapsed < 7.5, elapsed
            for stream in streams:
                assert stream.read(1) == b"", "idle timeout sent an extra response or did not close"
        finally:
            for stream in streams:
                stream.close()
            for client in clients:
                client.close()
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            with client.makefile("rb") as stream:
                client.sendall(get())
                assert read_frame(stream)[0] == 200
                before = time.monotonic()
                process.send_signal(signal.SIGTERM)
                assert process.wait(2) == 0, log.read_text()
                assert stream.read(1) == b"" and time.monotonic() - before < 2
    print("PASS: four idle workers released after 5 seconds, queued request resumes, no idle 408, immediate SIGTERM")


def check_request_limit():
    with proxy([]) as (port, _, _):
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            with client.makefile("rb") as stream:
                # 마지막 요청도 완전히 읽혔어도 100번째 응답 뒤에는 처리하지 않는다.
                client.sendall(get() * 101)
                for number in range(1, 101):
                    status, headers, body = read_frame(stream)
                    assert status == 200 and body == b"static stays local"
                    assert headers[b"connection"] == (b"close" if number == 100 else b"keep-alive")
                assert stream.read(1) == b"", "101st request on the same connection was processed"
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            with client.makefile("rb") as stream:
                client.sendall(get())
                assert read_frame(stream)[1][b"connection"] == b"keep-alive", "new connection inherited the old count"
    print("PASS: response 100 closes cleanly, buffered request 101 is ignored, new connection starts a fresh count")


def check_started_request_timeout():
    with proxy([]) as (port, _, _):
        with socket.create_connection(("127.0.0.1", port), timeout=8) as client:
            with client.makefile("rb") as stream:
                client.sendall(get())
                assert read_frame(stream)[0] == 200
                client.sendall(b"G")
                # 다음 요청이 시작됐으므로 5초가 아니라 기존 30초 무응답 제한을 적용한다.
                time.sleep(5.3)
                client.sendall(b"ET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                assert read_frame(stream)[0] == 200
                assert stream.read(1) == b""
    print("PASS: request already started remains valid beyond the 5-second between-request idle limit")


def check_proxy_framing(raw):
    with proxy([raw.server_address[1]]) as (port, _, _):
        for wire in (
            b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
            b"HTTP/1.1 204 No Content\r\n\r\n",
            b"HTTP/1.1 304 Not Modified\r\n\r\n",
        ):
            with raw.lock:
                raw.parts = [wire]
            with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                with client.makefile("rb") as stream:
                    client.sendall(get("/api/echo"))
                    assert read_frame(stream)[1][b"connection"] == b"keep-alive"
                    client.sendall(get(connection="close"))
                    assert read_frame(stream)[2] == b"static stays local"
                    assert stream.read(1) == b""
        for wire in (
            b"HTTP/1.0 200 OK\r\n\r\nhello",
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        ):
            with raw.lock:
                raw.parts = [wire]
            with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                client.sendall(get("/api/echo") + get())
                with client.makefile("rb") as stream:
                    result = stream.read()
                header, body = result.split(b"\r\n\r\n", 1)
                assert b"Connection: close" in header and body == b"hello", result
        with raw.lock:
            raw.parts = [b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort"]
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            client.sendall(get("/api/echo") + get())
            with client.makefile("rb") as stream:
                result = stream.read()
            assert result.count(b"HTTP/1.1 ") == 1 and result.endswith(b"short"), result
    print("PASS: length/bodyless upstream responses reuse frontend connection; EOF/dechunked responses close")


def main():
    with backend("A") as a, backend("B") as b, raw_backend() as raw:
        check_reuse(a, b)
        check_pipeline_and_versions()
        check_request_limit()
        check_idle_recovery_and_shutdown()
        check_started_request_timeout()
        check_proxy_framing(raw)


if __name__ == "__main__":
    main()
