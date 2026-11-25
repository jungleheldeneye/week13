/*
 * proxy.h — Reverse Proxy(경로/호스트 규칙 + Round-Robin)와 Forward Proxy
 *
 * 이 파일이 하는 일:
 *   1) -c 설정 파일을 읽어 라우팅 규칙을 만든다 (proxy_init)
 *   2) 요청이 어느 규칙에 맞는지 찾는다 (proxy_match)
 *   3) 백엔드(또는 원 서버)에 접속해 요청을 전달하고 응답을 클라이언트에 중계한다
 *
 * 설정 파일 예 (routes.conf):
 *   route /api/  127.0.0.1:8001 127.0.0.1:8002      ← /api/로 시작하면 두 백엔드에 번갈아
 *   route /b1/   127.0.0.1:8001 strip               ← strip: 앞부분(/b1)을 떼고 전달
 *   host  api.local  127.0.0.1:8001                 ← Host 헤더가 api.local이면
 */
#ifndef PROXY_H
#define PROXY_H

#include "config.h"
#include "http.h"

#define PROXY_MAX_ROUTES     32
#define PROXY_MAX_BACKENDS   8
#define PROXY_CONNECT_TIMEOUT 3    /* 백엔드 접속 대기 상한(초) */
#define PROXY_RESPONSE_TIMEOUT 10  /* 백엔드 응답 대기 상한(초): 넘으면 504 */
#define PROXY_DOWN_SECONDS   5     /* 접속 실패한 백엔드를 건너뛰는 시간(초) */

/*
 * proxy_init — 설정 파일을 읽는다. 경로가 비어 있으면 규칙 없이 초기화.
 *   결과: 0 성공, -1 실패 (err에 "몇 번째 줄이 왜 틀렸는지")
 */
int proxy_init(const char *path, char *err, int errlen);

/* proxy_route_count — 읽어 들인 규칙 수 (시작 화면 출력용) */
int proxy_route_count(void);

/* proxy_describe — i번째 규칙을 사람이 읽을 문장으로 (시작 화면 출력용) */
void proxy_describe(int i, char *out, size_t outlen);

/* proxy_match — 요청에 맞는 규칙 번호. 없으면 -1 (Host 규칙 우선, 그다음 가장 긴 경로 접두어) */
int proxy_match(const http_request_t *req);

/*
 * proxy_reverse / proxy_forward — 요청을 전달하고 응답을 중계한다.
 *   입력: fd(클라이언트 소켓), route(proxy_match 결과), req, client_ip, port(우리 서버 포트, 루프 감지용),
 *         keep(클라이언트가 keep-alive를 원하는지)
 *   결과: 클라이언트에 보낸 상태 코드 (502/504 등 포함), 중계 도중 끊겨 응답을 끝맺지 못하면 -1.
 *         *keep_out에는 이 연결을 계속 유지해도 되는지 (응답 끝을 확실히 알 때만 1)
 */
int proxy_reverse(int fd, int route, const http_request_t *req, const char *client_ip, int port,
                  int keep, int *keep_out, long long *sent);
int proxy_forward(int fd, const http_request_t *req, const char *client_ip, int port,
                  int keep, int *keep_out, long long *sent);

#endif
