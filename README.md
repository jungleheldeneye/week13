# cweb: C로 만든 HTTP 웹 서버 + Reverse/Forward Proxy

외부 라이브러리 없이 **POSIX 소켓 API만으로** 만든 HTTP/1.1 웹 서버입니다.
정적 파일, CGI, Reverse Proxy(로드 밸런싱), Forward Proxy(캐시)를 지원하고
동시 처리 방식 3가지(스레드 풀 / 연결마다 스레드 / 연결마다 프로세스)를 실행 옵션으로 바꿔 비교할 수 있습니다.

| 항목 | 내용 |
|---|---|
| 언어 / 의존성 | C11, 표준 C 라이브러리 + POSIX (pthread, socket, poll) |
| 지원 OS | macOS, Linux (WSL2 포함). 플랫폼 차이는 `#ifdef`로 분기 |
| 코드 규모 | 서버 약 3,600줄 (`src/`, 주석 포함), 테스트·도구 약 1,200줄 |
| 검증 | 자동 테스트 **253개** (단위 202 + CGI 17 + 프록시 13 + 예외 상황 21), ASan/LSan/TSan 경고 0 |
| 성능 (4코어 Linux) | 정적 파일 **약 22만 req/s** (keep-alive), 동시 1,000 연결 p99 24ms |

---

## 1. 빠르게 실행하기

```bash
make                                   # 빌드 → ./server
./server -p 8080 -r www                # http://localhost:8080
./server -h                            # 옵션 보기
```

| 옵션 | 뜻 | 기본값 |
|---|---|---|
| `-p <포트>` | 들을 포트 (필수) | |
| `-r <폴더>` | 정적 파일 루트 (필수) | |
| `-b <주소>` | 바인드 주소 | `0.0.0.0` |
| `-c <파일>` | Reverse Proxy 라우팅 규칙 파일 | 없음 |
| `-m pool\|thread\|fork` | 동시 처리 방식 | `pool` |
| `-t <수>` | 스레드 풀 워커 수 | CPU 코어 × 4 (4~64) |
| `-f` | Forward Proxy 허용 (+ 캐시) | 꺼짐 |

### 전체 시연 구성 (Reverse Proxy 포함)

```bash
./server -p 8001 -r demo/backend1 &      # 백엔드 1
./server -p 8002 -r demo/backend2 &      # 백엔드 2
./server -p 8080 -r www -c routes.conf -f   # 프론트 서버 (정적 + CGI + 프록시)
```

| 주소 | 보이는 것 |
|---|---|
| http://localhost:8080/ | 정적 파일 (HTML, CSS, JS, SVG) |
| http://localhost:8080/form.html | POST 폼 → `/echo` |
| http://localhost:8080/cgi.html | CGI 데모 (GET/POST, 스트리밍, 504/502/403) |
| http://localhost:8080/api/ | Reverse Proxy, 새로고침하면 백엔드 1 ↔ 2 번갈아 |
| `curl -x localhost:8080 http://localhost:8001/cgi-bin/time.sh` | Forward Proxy, 두 번째부터 `X-Cache: HIT` |

---

## 2. 명세 대응표

| 명세 항목 | 구현 | 위치 |
|---|---|---|
| socket → bind → listen → accept | `getaddrinfo`로 IPv4/IPv6 공통 처리, `SO_REUSEADDR`, listen 대기열 4096 | `net.c`, `server.c` |
| 요청 라인 / 헤더 / 본문 파싱 | GET·HEAD·POST, `Content-Length`와 `chunked` 본문, `Expect: 100-continue` | `http.c` |
| 응답 생성 | 상태 줄, `Content-Type`/`Content-Length`/`Connection`/`Date`/`Server`, 오류 페이지 | `response.c` |
| 정적 파일 + MIME | 20종 MIME, 폴더 → `index.html`, 301 리다이렉트, 304 Not Modified | `static_file.c` |
| 404 / 403 / 500 | 없음 404, 루트 밖·권한 없음 403, 그 밖의 파일 오류 500 | `static_file.c` |
| 동적 요청 | **CGI**(RFC 3875) + **백엔드 전달**(Reverse Proxy) + `/echo` | `cgi.c`, `proxy.c`, `echo.c` |
| 동시 처리 | **3가지 모두 구현**: 스레드 풀 + `poll` 대기실 / 연결마다 스레드 / 연결마다 `fork` | `pool.c`, `server.c` |
| Reverse Proxy | 경로·Host 규칙, 가장 긴 접두어 우선, `strip`, **Round-Robin**, 장애 전환, 502/504 | `proxy.c` |
| Forward Proxy | 절대 URL 요청 중계 + **응답 캐시** (LRU, `max-age`, `no-store`) | `proxy.c`, `cache.c` |
| CLI 인자 / 설정 파일 | 포트·루트·모드는 CLI, 라우팅은 `routes.conf` | `config.c` |
| SIGINT/SIGTERM, SIGPIPE | 종료 플래그 → 진행 중 요청 마무리 후 종료, SIGPIPE 무시 | `server.c` |
| 단위 / 기능 / 부하 / 누수 테스트 | `make check`, `tests/bench.sh`, ASan/LSan/TSan | `tests/`, `tools/` |

---

## 3. 구조

```
                    ┌───────────── cweb (프론트 서버) ─────────────┐
 브라우저/curl ──▶  │ accept → 대기실(poll) → 워커 → 요청 파싱      │
                    │                         │                     │
                    │   라우팅 우선순위:       ▼                     │
                    │   ① routes.conf 규칙 ──▶ Reverse Proxy ──────┼──▶ 백엔드 8001 / 8002 (Round-Robin)
                    │   ② 절대 URL + -f    ──▶ Forward Proxy ──────┼──▶ 원 서버 (캐시 HIT면 바로 응답)
                    │   ③ /cgi-bin/        ──▶ fork + exec ────────┼──▶ 스크립트 (환경 변수 + 파이프)
                    │   ④ /echo            ──▶ 본문 그대로 응답     │
                    │   ⑤ 나머지           ──▶ 정적 파일            │
                    └───────────────────────────────────────────────┘
```

| 파일 | 역할 |
|---|---|
| `main.c`, `config.c` | 옵션 해석, 시작 정보 출력 |
| `net.c` | 소켓 생성/접속, 버퍼형 읽기, 부분 쓰기 처리(`write_all`, `writev_all`), 시간 제한 |
| `http.c` | 요청 파싱, URL 디코딩, 본문(Content-Length/chunked) 읽기 |
| `response.c` | 응답 헤더, 오류 페이지, MIME, 날짜 |
| `conn.c` | 연결 하나에서 요청 하나 처리 + 라우팅 + 접근 로그 |
| `server.c`, `pool.c` | 시그널, accept, 동시 처리 3가지 |
| `static_file.c`, `echo.c`, `cgi.c` | 응답 생성기 |
| `proxy.c`, `cache.c` | Reverse/Forward Proxy, 캐시 |

---

## 4. 설계 결정과 트레이드오프

### 동시 처리: 왜 "스레드 풀 + poll 대기실"이 기본인가
- **연결마다 스레드/프로세스**는 단순하지만, keep-alive로 놀고 있는 연결도 스레드를 하나씩 붙잡습니다. 연결이 만 개면 스레드도 만 개입니다 (C10K 문제).
- **스레드 풀만** 쓰면 놀고 있는 연결이 워커를 붙잡아 다른 요청이 굶습니다.
- 그래서 **요청이 실제로 도착한 연결만** 워커에게 넘기고, 응답 후 다음 요청을 기다리는 연결은 `poll` 대기실로 돌려보냅니다. 워커 16개로 수천 개의 연결을 유지할 수 있습니다.
- 측정해 보면 keep-alive 처리량은 연결마다 스레드 방식이 조금 높습니다 (대기실 왕복 비용). 대신 새 연결 처리량은 스레드 풀이 1.7배, fork보다 4배 빠르고, 놀고 있는 연결이 많아도 버팁니다.

### Forward Proxy vs Reverse Proxy
| | Forward Proxy | Reverse Proxy |
|---|---|---|
| 누구를 대신하나 | **클라이언트**를 대신해 외부 서버에 요청 | **서버들**을 대신해 요청을 받음 |
| 누가 설정하나 | 클라이언트 (`curl -x`, 브라우저 프록시 설정) | 서버 운영자 (`routes.conf`) |
| 보안 | 내부 사용자의 외부 접근 통제 | 백엔드 주소를 숨김, 입구를 하나로 |
| 로드 밸런싱 | 해당 없음 | Round-Robin + 장애 백엔드 5초 제외 |
| 캐싱 | 여러 사용자가 같은 외부 응답을 공유 (구현함) | 백엔드 부하 감소 (확장 가능) |

### CGI: fork + exec + 파이프 2개
- 요청 정보는 환경 변수(`QUERY_STRING`, `CONTENT_LENGTH`, `HTTP_*`)로, 본문은 표준 입력으로 넘깁니다.
- 본문 쓰기와 출력 읽기를 `poll`로 **동시에** 해서, 큰 POST에서 서로 기다리다 멈추는 교착을 막습니다.
- 출력은 생기는 대로 `chunked`로 흘려보냅니다 (스트리밍). 10초 넘으면 강제 종료 + 504, 헤더가 없으면 502.

---

## 5. 테스트

```bash
make check        # 아래 4가지 모두 (약 30초)
make test         # 단위 테스트 202개: 설정, 소켓 읽기, HTTP 파서, 본문, 정적 파일, 라우팅, 캐시, CGI 헤더
make cgitest      # CGI 통합 17개: GET/POST/PATH_INFO, 403/404/502/504, 경로 탈출, httpoxy, 3MB 왕복
make proxytest    # 프록시 통합 13개: Round-Robin 2:2, 장애 전환, 전부 다운 502, 캐시 MISS→HIT, 루프 508
make edgetest     # 예외 상황 21개 (python3): 아래 표
```
테스트는 시연 서버(8001, 8002, 8080)와 겹치지 않는 포트(8011, 8012, 8091, 8097~8099)를 씁니다.

| 예외 상황 (명세) | 확인한 것 |
|---|---|
| 잘못된 HTTP 요청 | 모르는 메서드 501, 잘못된 버전 505/400, 콜론 없는 헤더 400, Host 없음 400, `%00` 400 |
| 요청 밀반입 공격 | `Content-Length` + `Transfer-Encoding` 동시 → 400 |
| 매우 큰 본문 / 긴 URI | 8MB 초과 413 (읽기 전에 거절), URI 414, 헤더 431 |
| 백엔드 응답 없음 / 타임아웃 | 꺼진 백엔드 502, 응답 안 하는 백엔드 10초 후 504 |
| 갑작스러운 연결 종료 | 요청/본문/20MB 다운로드/CGI 스트리밍 중간에 120번 끊어도 서버 정상 |
| 느린 공격 (slowloris) | 요청 하나당 30초 마감 → 408 |

**메모리 검사**: `make asan` (AddressSanitizer + LeakSanitizer + UBSan), `make tsan` (ThreadSanitizer).
`SERVER=./server_asan python3 tests/edge_test.py`로 예외 상황을 돌린 뒤 종료 시 누수 보고 0을 확인합니다.
macOS는 LeakSanitizer가 없어서 `leaks --atExit -- ./server ...`로 확인할 수 있습니다.

---

## 6. 부하 테스트

```bash
make bench && bash tests/bench.sh        # 모드별 표 (직접 만든 bench 도구)
ab -n 20000 -c 50 -k http://127.0.0.1:8080/index.html   # macOS 기본 ab로도 가능
```

`tools/bench.c`는 ab 대용으로 직접 만든 도구입니다. ab는 HTTP/1.0만 써서 CGI의 chunked + keep-alive 응답을 측정하지 못해서, HTTP/1.1 응답(Content-Length / chunked / 연결 종료)을 모두 처리하도록 만들었습니다.

측정 환경: Linux VM 4코어, 동시 50 연결, 같은 기기에서 클라이언트와 서버를 함께 실행.
같은 조건이어도 실행할 때마다 ±20% 정도 차이가 나므로 순위와 배수 위주로 봐 주세요. macOS는 기본 fd 한도가 256이라 `bench.sh`가 시작할 때 `ulimit -n`을 올립니다.

| 모드 | 정적 keep-alive | 정적 새 연결 | CGI | Reverse Proxy |
|---|---|---|---|---|
| pool (기본) | 169,252 req/s | **118,891** | 4,254 | 74,418 |
| thread | 227,495 | 70,909 | 2,904 | 84,901 |
| fork | 223,457 | 30,095 | 6,619 | 89,374 |

| 동시 연결 (pool, keep-alive) | 1 | 10 | 50 | 200 | 1,000 |
|---|---|---|---|---|---|
| 초당 요청 | 23,473 | 140,291 | 219,354 | 240,000 | 194,322 |
| p99 응답 시간 | 0.05ms | 0.17ms | 0.69ms | 2.35ms | 24ms |

읽는 법:
- 새 연결은 pool이 가장 빠릅니다. 스레드/프로세스를 매번 만들지 않기 때문입니다.
- CGI는 요청마다 프로세스를 만들어서 정적 파일보다 약 40배 느립니다. fork 모드가 가장 빠른 이유는 스레드가 하나뿐인 프로세스에서 fork하는 비용이 더 싸기 때문입니다.

### 측정으로 찾아서 고친 문제 3가지
| 증상 | 원인 | 조치 | 결과 |
|---|---|---|---|
| CGI 응답마다 정확히 40ms 지연 | Nagle 알고리즘 + 클라이언트의 지연 ACK | `TCP_NODELAY`, 조각 전송을 `writev` 한 번으로 | CGI **22 → 976 req/s** (동시 1) |
| 동시 1,000 연결에서 p99가 1초 | listen 대기열 128이 넘쳐 SYN이 버려지고 1초 뒤 재전송 | 대기열 4096 | p99 **1,262ms → 24ms** |
| ulimit이 크면 CGI가 느림 | 자식에서 fd를 한도(수십만)까지 하나씩 close | Linux는 `close_range` 시스템 콜 | 자식 정리 1회 호출로 끝남 |

---

## 7. 보안 / 안정성 처리 모음

- **경로 탈출 차단**: `realpath`로 실제 경로를 구한 뒤 루트 안인지 검사 (`/../`, `%2e%2e`, 심볼릭 링크)
- **CGI**: `cgi-bin` 밖 실행 금지, 실행 권한 검사, httpoxy 차단(`Proxy:` 헤더 전달 안 함), 자식에서 서버 소켓 모두 닫기, 서버 환경 변수를 통째로 넘기지 않기
- **프록시**: hop-by-hop 헤더 제거, `X-Forwarded-For/Proto/Host`, `Via`로 루프 감지 → 508
- **자원 제한**: 본문 8MB, 헤더 16KB, 연결당 요청 100개, 유휴 5초, 요청 마감 30초, 파일 디스크립터 고갈(`EMFILE`) 시 accept 1초 쉬기, 대기열 가득 차면 503
- **종료**: SIGINT/SIGTERM → 새 연결 중단, 진행 중 요청 마무리, 워커 join, 메모리 반환

---

## 8. 한계와 다음 단계

- **Forward Proxy(`-f`)는 열린 프록시**입니다. 누구나 이 서버를 거쳐 다른 주소(내부망 포함)에 요청할 수 있으므로, 켤 때는 `-b 127.0.0.1`로 내 컴퓨터에서만 쓰거나 신뢰할 수 있는 네트워크에서만 쓰세요.

- HTTPS(TLS), HTTP/2 미지원. Forward Proxy의 `CONNECT` 터널도 미지원 (http:// 만 중계)
- 프록시가 백엔드와 요청마다 새 연결을 맺음 → 연결 재사용(풀)을 넣으면 프록시 처리량이 더 오를 여지
- `poll`은 연결 수에 비례해 비용이 늘어남 → `epoll`(Linux) / `kqueue`(macOS)로 바꾸면 수만 연결까지 확장
- 캐시는 프로세스 메모리에 있어서 fork 모드에서는 공유되지 않음

---

## 폴더 구조

```
src/            서버 코드 (모듈별 .c/.h, 모든 함수에 한글 주석)
tests/          단위 테스트(C) + 통합 테스트(bash, python) + 부하 표(bench.sh)
tools/bench.c   부하 테스트 도구
www/            정적 파일 + cgi-bin 데모 스크립트
demo/           Reverse Proxy 시연용 백엔드 두 개
routes.conf     Reverse Proxy 규칙 예시
DEMO.md         발표 시연 순서
REVIEW.md       검수 요청서
docs/           과제 명세
```
