# 검수 요청서

C로 만든 HTTP 웹 서버(+ Reverse/Forward Proxy, CGI)입니다. 아래 순서대로 봐 주시면 됩니다.
기준 명세는 같은 폴더의 `docs/명세_웹서버만들기.md`입니다.

## 0. 준비 (5분)
- 필요한 것: macOS 또는 Linux(WSL2 가능), `cc`(gcc/clang), `make`, `curl`, `python3`
- 압축을 푼 뒤 터미널에서:
```bash
cd WebServer
chmod +x www/cgi-bin/*.sh demo/backend1/cgi-bin/*.sh demo/start.sh   # zip이 실행 권한을 잃었을 때 대비
```

## 1. 돌려 보기 (10분) — 꼭 부탁드려요
```bash
make clean && make check     # 자동 테스트 253개 (약 30초). 마지막 줄들이 모두 "0개 실패"인지
make asan && SERVER=./server_asan python3 tests/edge_test.py   # 메모리 오류/누수 보고 0인지 (Linux 권장)
bash demo/start.sh           # 서버 3개 실행 → 브라우저 http://localhost:8080 (종료: Ctrl+C)
```
서버를 켜 둔 상태에서 `DEMO.md` 순서대로 시연이 되는지 봐 주세요.

## 2. 명세 대응 (10분) — 꼭 부탁드려요
- `README.md` 2절 "명세 대응표"를 `docs/명세_웹서버만들기.md`와 비교해서 빠졌거나 과장된 항목이 있는지
- README의 숫자(테스트 수, 성능 표)가 실제 실행 결과와 크게 다르지 않은지

## 3. 코드 리뷰 (가능하면) — 위험도 높은 곳 위주
| 파일 | 봐 줄 것 |
|---|---|
| `src/http.c` | 버퍼 경계(긴 URI/헤더), Content-Length·chunked 처리, 잘못된 입력에서 크래시 |
| `src/pool.c` | 대기열/반환함 잠금, 종료 시 워커 join, 연결 누수 |
| `src/cgi.c` | fork 후 자식 코드의 안전성, 파이프 교착, 시간 초과 시 좀비 |
| `src/proxy.c`, `src/cache.c` | 백엔드 타임아웃 502/504, 캐시 잠금 범위, 저장 조건 |
| `src/static_file.c` | 경로 탈출(`..`, `%2e%2e`, 심볼릭 링크) 차단 |

## 4. 직접 깨 보기 (선택)
- `nc localhost 8080`으로 이상한 요청을 직접 입력, 중간에 끊기, 큰 파일 업로드 → 서버가 죽거나 멈추는지
- `ab -n 20000 -c 200 -k http://127.0.0.1:8080/index.html` 후에도 정상 응답하는지

## 알려진 한계 (이미 알고 있음)
- HTTPS, HTTP/2, Forward Proxy의 CONNECT 미지원
- 서버가 시작할 때 fd 한도를 올리지 않음 (macOS 기본 256 → 동시 수백 연결 부하 시 지연 가능)
- 캐시 HIT 응답에 원 서버의 Age 헤더가 있으면 Age가 두 번 들어감

## 피드백 형식
`파일:줄 / 재현 방법 / 기대 결과 / 실제 결과` 로 적어 주시면 바로 고치겠습니다.
