# C 웹 서버

POSIX 소켓 기반 HTTP/1.0·1.1 서버. GET·POST, 정적 파일·MIME 응답, 경로 기반 리버스 프록시 지원.

## 실행

```sh
cd webserver
make
./webserver --port 8080 --root ./www
```

- 리스닝 주소 `127.0.0.1` (루프백).
- 기본값은 포트 `8080`, 문서 루트 `./www`. 포트 `0`은 OS 자동 할당.
- 정적 응답 확인은 `curl -i http://127.0.0.1:8080/`.

## 리버스 프록시

`webserver/`에서 각 명령을 별도 터미널로 실행.

```sh
python3 tests/backend.py --port 8001 --name A
python3 tests/backend.py --port 8002 --name B
./webserver --port 8080 --root ./www \
  --backend 127.0.0.1:8001 --backend 127.0.0.1:8002
```

- `/api`와 `/api/*`는 백엔드 A·B에 라운드 로빈 분산. 경로·쿼리·본문 유지.
- `--proxy-prefix`로 접두사 변경, `--proxy-timeout`으로 백엔드 무응답 시간 변경. 기본 5,000ms.
- `curl -i http://127.0.0.1:8080/api/echo` 반복으로 `backend` 값 확인.
- `curl -i --data-binary 'hello' http://127.0.0.1:8080/api/echo`로 POST 중계 확인.

## 처리 정책

| 항목 | 설정 |
| --- | --- |
| 동시 처리 | 워커 4개 · FIFO 연결 대기 64개 · 포화 시 `503` 시도 후 연결 종료 |
| 요청 상한 | URI 2KiB · 헤더 16KiB 및 100개 · POST 본문 1MiB |
| 본문 경계 | 요청은 `Content-Length` 기반 · POST 길이 누락 시 `411` · chunked 요청은 `501` |
| 연결 재사용 | HTTP/1.1 Keep-Alive · 다음 요청 대기 5초 · 연결당 최대 100개 |
| 응답 전송 | 정적 파일과 프록시 응답 본문을 최대 16KiB씩 전송 |
| 백엔드 오류 | 연결·응답 오류 `502` · 백엔드 미설정 `503` · 응답 시작 전 타임아웃 `504` |
| 종료 | `SIGINT`·`SIGTERM`에서 연결 즉시 정리 · `SIGPIPE` 무시 |

## 검증

| 명령 | 확인 항목 |
| --- | --- |
| `make test` | HTTP 파서, MIME·라우팅, CLI, 스레드 풀, 프록시 단위 테스트 |
| `make integration-test` | 실제 TCP 정적 GET·POST·라운드 로빈·Keep-Alive·오류·종료 |
| `make sanitize-test sanitize-integration-test` | ASan·UBSan 단위·통합 검사 |

macOS arm64 `leaks` 결과는 실행 중 및 종료 시 **누수 0건, 0바이트**.

## 부하 테스트 결과

macOS arm64 루프백, 4KiB 본문, 요청마다 TCP 연결 생성. 처리량 단위는 성공 요청/초.

| 동시 접속 시도 | 정적 처리량 / p95 / 503 | 프록시 처리량 / p95 / 503 |
| ---: | ---: | ---: |
| 1 | 8,613 / 0.2ms / 0% | 3,941 / 0.4ms / 0% |
| 64 | 14,979 / 10.9ms / 0% | 6,504 / 27.1ms / 0% |
| 96 | 17,878 / 14.1ms / 3.7% | 2,473 / 30.2ms / 88.0% |
| 192 | 21,692 / 13.8ms / 2.0% | 561 / 326.7ms / 97.3% |

## C10K와 스레드 풀의 한계

유휴 Keep-Alive와 백엔드 응답 대기도 워커 점유. 1만 연결 목표에서는 `epoll`·`kqueue`, 논블로킹 I/O, 연결별 상태·메모리 상한 검토. [설계 결정 기록](decisions.md)
