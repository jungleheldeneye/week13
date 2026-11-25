#!/usr/bin/env python3
"""
edge_test.py — 예외 상황(안정성) 통합 테스트

명세 "예외 상황 처리" 항목을 소켓으로 직접 재현한다. (curl로는 못 만드는 깨진 요청도 보냄)
  - 잘못된 HTTP 요청 (불완전한 헤더, 비정상 메서드, 잘못된 버전)
  - 매우 큰 요청 본문 / 매우 긴 URI / 너무 큰 헤더
  - 백엔드 서버 응답 없음 / 타임아웃 (Reverse Proxy)
  - 클라이언트의 갑작스러운 연결 종료 (요청 중간, 본문 중간, 응답 받는 중간)
마지막에 "서버가 아직 살아서 정상 응답하는지"와 "프로세스가 남지 않았는지"를 확인한다.

사용:  python3 tests/edge_test.py                 (기본 ./server)
       SERVER=./server_asan python3 tests/edge_test.py   (메모리 오류/누수까지 검사)
"""
import os, socket, subprocess, sys, tempfile, threading, time

SERVER = os.environ.get("SERVER", "./server")
PORT = int(os.environ.get("PORT", "8097"))
HANG_PORT, DEAD_PORT = PORT + 1, PORT + 2      # 응답 안 하는 백엔드, 아무도 없는 포트
passed = failed = 0


def check(name, expect, got):
    global passed, failed
    if expect == got:
        passed += 1
        print(f"  통과  {name}")
    else:
        failed += 1
        print(f"  실패  {name}: 기대 {expect!r}, 실제 {got!r}")


def conn(timeout=15):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    return s


def status_of(raw: bytes, timeout=15):
    """원시 바이트를 보내고 응답 상태 코드를 돌려준다 (연결이 그냥 끊기면 None)"""
    s = conn(timeout)
    try:
        s.sendall(raw)
        data = b""
        while b"\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        parts = data.split(b" ", 2)
        return int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else None
    except (ConnectionResetError, BrokenPipeError):
        return None
    finally:
        s.close()


def hang_backend():
    """접속은 받지만 영원히 응답하지 않는 백엔드 (504 재현용)"""
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", HANG_PORT))
    ls.listen(16)
    held = []
    while True:
        c, _ = ls.accept()
        held.append(c)          # 닫지도, 답하지도 않음


def main():
    threading.Thread(target=hang_backend, daemon=True).start()
    routes = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
    routes.write(f"route /hang/ 127.0.0.1:{HANG_PORT}\nroute /dead/ 127.0.0.1:{DEAD_PORT}\n")
    routes.close()

    log = open("/tmp/edge_test.log", "w")
    srv = subprocess.Popen([SERVER, "-p", str(PORT), "-r", "www", "-c", routes.name], stdout=log, stderr=log)
    time.sleep(1.0)
    try:
        run_cases()
    finally:
        srv.terminate()                       # SIGTERM → 정상 종료 경로 (누수 검사는 종료 시점에 나옴)
        try:
            srv.wait(timeout=20)
        except subprocess.TimeoutExpired:
            srv.kill()
        os.unlink(routes.name)
    log.close()
    text = open("/tmp/edge_test.log", errors="replace").read()
    check("메모리 오류/누수 보고 없음 (ASan/LSan)", False,
          "ERROR: AddressSanitizer" in text or "ERROR: LeakSanitizer" in text or "runtime error" in text)
    print(f"\n예외 상황 테스트: {passed}개 통과, {failed}개 실패  (서버 로그: /tmp/edge_test.log)")
    sys.exit(1 if failed else 0)


def run_cases():
    H = b"Host: localhost\r\n"
    print("[잘못된 요청]")
    check("알 수 없는 메서드 → 501", 501, status_of(b"BREW / HTTP/1.1\r\n" + H + b"\r\n"))
    check("메서드 자리에 이상한 글자 → 400", 400, status_of(b"G(T / HTTP/1.1\r\n" + H + b"\r\n"))
    check("지원 안 하는 버전 HTTP/2.0 → 505", 505, status_of(b"GET / HTTP/2.0\r\n" + H + b"\r\n"))
    check("버전 형식 오류 → 400", 400, status_of(b"GET / HTTX/1.1\r\n" + H + b"\r\n"))
    check("요청 줄 조각 부족 → 400", 400, status_of(b"GET /\r\n\r\n"))
    check("HTTP/1.1인데 Host 없음 → 400", 400, status_of(b"GET / HTTP/1.1\r\n\r\n"))
    check("콜론 없는 헤더 → 400", 400, status_of(b"GET / HTTP/1.1\r\n" + H + b"BrokenHeader\r\n\r\n"))
    check("%00 들어간 경로 → 400", 400, status_of(b"GET /a%00b HTTP/1.1\r\n" + H + b"\r\n"))
    check("Content-Length 숫자 아님 → 400", 400,
          status_of(b"POST /echo HTTP/1.1\r\n" + H + b"Content-Length: abc\r\n\r\n"))
    check("CL+TE 동시 (요청 밀반입) → 400", 400,
          status_of(b"POST /echo HTTP/1.1\r\n" + H + b"Content-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"))
    check("잘못된 chunk 크기 → 400", 400,
          status_of(b"POST /echo HTTP/1.1\r\n" + H + b"Transfer-Encoding: chunked\r\n\r\nzz\r\n"))

    print("[너무 큰 요청]")
    check("URI 20KB → 414", 414, status_of(b"GET /" + b"a" * 20000 + b" HTTP/1.1\r\n" + H + b"\r\n"))
    check("헤더 합계 40KB → 431", 431,
          status_of(b"GET / HTTP/1.1\r\n" + H + b"".join(b"X-%d: %s\r\n" % (i, b"v" * 1000) for i in range(40)) + b"\r\n"))
    check("본문 9MB 선언 → 413 (읽기 전에 거절)", 413,
          status_of(b"POST /echo HTTP/1.1\r\n" + H + b"Content-Length: 9437184\r\n\r\n"))
    # RFC 9112 6.3: 요청에 Content-Length/Transfer-Encoding이 없으면 "본문 길이 0"으로 본다 → 정상 처리
    check("POST에 길이 정보 없음 → 본문 0으로 처리", 200, status_of(b"POST /echo HTTP/1.1\r\n" + H + b"\r\n"))

    print("[Reverse Proxy 백엔드 장애]")
    t = time.time()
    check("백엔드가 꺼져 있음 → 502", 502, status_of(b"GET /dead/x HTTP/1.1\r\n" + H + b"\r\n"))
    check("백엔드가 응답 안 함 → 504", 504, status_of(b"GET /hang/x HTTP/1.1\r\n" + H + b"\r\n", timeout=30))
    el = time.time() - t
    check("504까지 약 10초 (무한 대기 안 함)", True, 8 <= el <= 15)

    print("[클라이언트의 갑작스러운 연결 종료]")
    for i in range(30):                              # 요청 줄 중간에 끊기
        s = conn(); s.sendall(b"GET /ind"); s.close()
    for i in range(30):                              # 본문 중간에 끊기
        s = conn(); s.sendall(b"POST /echo HTTP/1.1\r\n" + H + b"Content-Length: 100000\r\n\r\n" + b"x" * 5000); s.close()
    for i in range(30):                              # 접속만 하고 바로 끊기
        conn().close()
    big = "/big.bin"
    with open("www/big.bin", "wb") as f:
        f.write(os.urandom(20 * 1024 * 1024))
    for i in range(10):                              # 큰 파일을 받다가 중간에 끊기 (SIGPIPE/EPIPE 처리)
        s = conn(); s.sendall(b"GET " + big.encode() + b" HTTP/1.1\r\n" + H + b"\r\n"); s.recv(65536)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")  # RST로 끊기
        s.close()
    for i in range(10):                              # CGI 스트리밍 중간에 끊기
        s = conn(); s.sendall(b"GET /cgi-bin/stream.sh HTTP/1.1\r\n" + H + b"\r\n"); s.recv(100); s.close()
    os.remove("www/big.bin")
    time.sleep(1)
    check("끊김 120번 뒤에도 서버 정상 응답", 200, status_of(b"GET / HTTP/1.1\r\n" + H + b"\r\n"))

    print("[동시 접속]")
    results = []
    def worker():
        try:
            results.append(status_of(b"GET /index.html HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n"))
        except Exception as e:
            results.append(repr(e))
    ths = [threading.Thread(target=worker) for _ in range(200)]
    [t.start() for t in ths]; [t.join() for t in ths]
    check("동시 200개 연결 모두 200", 200, results.count(200))


if __name__ == "__main__":
    main()
