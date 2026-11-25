/*
 * cgi.h — CGI 실행 (동적 요청 처리)
 *
 * 이 파일이 하는 일:
 *   /cgi-bin/ 아래 경로로 요청이 오면, 그 파일을 "보내는" 대신 "실행"하고 출력을 응답으로 보낸다.
 *     - 요청 정보 → 환경 변수 (REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH, HTTP_* ...)
 *     - 요청 본문 → 프로그램의 표준 입력
 *     - 프로그램의 표준 출력 → "헤더 + 빈 줄 + 본문" → 서버가 상태 줄을 붙여 클라이언트에 전달
 *
 * 예: GET /cgi-bin/hello.sh/extra?name=hyeok
 *     → SCRIPT_NAME=/cgi-bin/hello.sh, PATH_INFO=/extra, QUERY_STRING=name=hyeok
 */
#ifndef CGI_H
#define CGI_H

#include "config.h"
#include "http.h"

#define CGI_PREFIX       "/cgi-bin/"
#define CGI_TIMEOUT_S    10        /* 스크립트 실행 시간 상한: 넘으면 강제 종료 + 504 */
#define CGI_MAX_HEADER   16384     /* 스크립트가 출력하는 헤더 부분 상한: 넘으면 502 */

/* cgi_is_request — 경로가 CGI 대상인지 (/cgi-bin/ 으로 시작) */
int cgi_is_request(const http_request_t *req);

/*
 * cgi_serve — 스크립트를 실행하고 출력을 응답으로 중계한다.
 *   입력: fd(클라이언트), cfg, req, peer("IP:포트"), keep(클라이언트가 keep-alive를 원하는지)
 *   결과: 응답 상태 코드(로그용), 응답을 끝맺지 못하고 끊어야 하면 -1.
 *         *keep_out = 연결을 유지해도 되는지, *sent = 보낸 본문 바이트
 */
int cgi_serve(int fd, const server_config_t *cfg, const http_request_t *req, const char *peer,
              int keep, int *keep_out, long long *sent);

/*
 * cgi_parse_headers — 스크립트 출력의 헤더 부분("Content-Type: ...\nStatus: 404\n")을 해석한다.
 *   (cgi_serve 내부용이지만 단위 테스트를 위해 공개)
 *   입력: hdr(수정됨), out = 클라이언트에 보낼 헤더 줄들, reason = 상태 이름
 *   결과: 상태 코드 (Status 없으면 200, Location만 있으면 302), 형식이 틀리면 -1
 */
int cgi_parse_headers(char *hdr, char *out, size_t outlen, char *reason, size_t rlen);

#endif
