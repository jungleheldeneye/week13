# 발표 시연 순서 (4분 + QnA 1분)

준비: 터미널 2개, 브라우저 1개
```bash
bash demo/start.sh          # 터미널 1: 서버 3개 (접근 로그가 여기 찍힘)
```

## 0:00 ~ 0:30  무엇을 만들었나 (README 1절 표)
- "외부 라이브러리 없이 POSIX 소켓만으로 HTTP/1.1 서버를 만들었고, 정적 파일, CGI, Reverse/Forward Proxy, 동시 처리 3가지를 구현했습니다."
- "자동 테스트 253개와 Sanitizer로 검증했습니다."

## 0:30 ~ 1:30  기능 시연 (브라우저 + 터미널 1의 로그를 같이 보여 주기)
1. http://localhost:8080/ : 정적 파일. 로그에 css/js/svg 요청이 **같은 포트(keep-alive)** 로 찍힘
2. http://localhost:8080/cgi.html → 이름 입력: CGI GET. "스트리밍" 링크: 1초마다 한 줄씩 도착
3. http://localhost:8080/api/ 새로고침 2번: 백엔드 1 ↔ 2 번갈아 (Round-Robin)
4. 터미널 2:
```bash
curl -x localhost:8080 -sD- http://localhost:8001/cgi-bin/time.sh | grep -E 'X-Cache|시각'
curl -x localhost:8080 -sD- http://localhost:8001/cgi-bin/time.sh | grep -E 'X-Cache|시각'
```
→ 첫 번째 MISS, 두 번째 HIT + 시각이 같음 (Forward Proxy 캐시)

## 1:30 ~ 2:30  검증 (README 5절)
```bash
make proxytest    # 약 3초, Round-Robin 2:2, 장애 전환, 502, 캐시, 508 (시연 서버와 다른 포트라 켜 둔 채로 실행 가능)
```
- 예외 상황 표를 짚으며: "깨진 요청, 큰 본문, 백엔드 타임아웃, 갑작스러운 연결 종료를 python으로 직접 만들어 21개 확인했습니다."
- 시간이 남으면 `curl -s localhost:8080/cgi-bin/slow.sh` → 10초 뒤 504 (기다리는 동안 다음 이야기)

## 2:30 ~ 3:30  설계와 측정 (README 4절, 6절)
- 동시 처리: "놀고 있는 keep-alive 연결이 워커를 붙잡지 않도록 poll 대기실을 두었습니다."
- 부하 표: "새 연결은 스레드 풀이 fork보다 4배 빠르고, keep-alive는 22만 req/s까지 나옵니다."
- **측정으로 찾은 문제 3가지 표**: "CGI가 정확히 40ms씩 느렸는데 Nagle + 지연 ACK 때문이었고, TCP_NODELAY로 45배 빨라졌습니다."

## 3:30 ~ 4:00  한계와 배운 점 (README 8절)
- HTTPS/CONNECT 없음, 백엔드 연결 재사용 없음, poll → epoll/kqueue 확장 여지
- "AI가 쓴 코드를 테스트와 측정으로 검증하면서, 숫자가 이상한 곳에서 실제 버그를 찾았습니다."

---

## 예상 질문과 짧은 답

| 질문 | 답 |
|---|---|
| keep-alive를 어떻게 구현했나? | 응답 후 연결을 닫지 않고 대기실(poll)에 넣음. 다음 요청이 오면 다시 워커에게. 유휴 5초, 연결당 100개 제한 |
| 요청 본문의 끝은 어떻게 아나? | `Content-Length`만큼 읽거나, `chunked`면 크기 0 조각까지 읽음. 둘 다 오면 공격으로 보고 400 |
| 클라이언트가 중간에 끊으면? | SIGPIPE를 무시해서 write가 오류(EPIPE)로 돌아오고, 그 연결만 정리. 120번 끊는 테스트로 확인 |
| 왜 epoll이 아니라 poll? | macOS(kqueue)와 Linux(epoll)가 달라서 POSIX 표준인 poll을 택함. 수만 연결이면 바꿀 부분 |
| CGI에서 교착이 뭔가? | 서버는 본문을 다 쓰려 하고, 스크립트는 출력이 꽉 차서 멈추면 서로 기다림. poll로 쓰기·읽기를 동시에 해서 해결 |
| Round-Robin 순번은 fork 모드에서도 맞나? | 순번을 공유 메모리(mmap MAP_SHARED)에 둬서 프로세스끼리 공유 |
| 캐시가 잘못된 응답을 주지 않나? | no-store/private/Set-Cookie/Vary/Authorization은 저장 안 함, max-age 지나면 버림, 요청이 no-cache면 새로 받음 |
| 안전한 종료는? | 시그널 핸들러는 플래그만 세움. 메인이 accept를 멈추고 진행 중 요청을 마친 뒤 워커를 join |
