/*
 * http.h — HTTP 요청 해석(파싱)
 *
 * 이 파일이 하는 일:
 *   소켓에서 읽은 요청 줄과 헤더 줄을 검사해서 http_request_t 구조체로 바꾼다.
 *   잘못된 요청이면 "어떤 상태 코드로 거절할지"(400/501/505/431...)를 돌려준다.
 *
 *   요청 예:  GET /docs/a%20b.html?x=1 HTTP/1.1\r\n
 *            Host: localhost:8080\r\n
 *            \r\n
 *   결과:    method="GET", path="/docs/a b.html", query="x=1", minor=1, host 헤더 1개
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include "net.h"

#define HTTP_MAX_HEADERS      64      /* 헤더 개수 상한 (넘으면 431) */
#define HTTP_MAX_HEADER_BYTES 16384   /* 헤더 전체 크기 상한 (넘으면 431) */
#define HTTP_MAX_TARGET       4096    /* 요청 대상(URL) 길이 상한 (넘으면 414) */
#define HTTP_MAX_BODY         (8 * 1024 * 1024)  /* 요청 본문 크기 상한 8MB (넘으면 413) */

typedef enum { HTTP_GET, HTTP_HEAD, HTTP_POST } http_method_t;

typedef struct {
    const char *name;    /* hbuf 안을 가리킴 (따로 메모리 할당 없음) */
    const char *value;
} http_header_t;

typedef struct {
    /* 요청 줄 */
    http_method_t method;
    char method_str[16];           /* "GET" 등 원문 (로그용) */
    char target[HTTP_MAX_TARGET];  /* 원문 대상: "/a%20b?x=1" 또는 "http://host/a" */
    char path[HTTP_MAX_TARGET];    /* %XX 해석된 경로: "/a b" */
    char query[HTTP_MAX_TARGET];   /* '?' 뒤 원문: "x=1" (CGI의 QUERY_STRING으로 쓰임) */
    int  minor;                    /* HTTP/1.0 → 0, HTTP/1.1 → 1 */

    /* 절대 URL 요청(http://host:port/path, 프록시가 받는 형태)일 때만 채워짐 */
    int  is_absolute;
    char abs_host[256];
    int  abs_port;

    /* 본문 (http_read_body가 채움, 다 쓰면 http_free_body로 반납) */
    char  *body;                   /* malloc한 본문, 끝에 '\0'을 붙여 둠 (문자열처럼 써도 안전) */
    size_t body_len;

    /* 헤더 */
    http_header_t headers[HTTP_MAX_HEADERS];
    int    nheaders;
    char   hbuf[HTTP_MAX_HEADER_BYTES];  /* 헤더 이름/값 문자열을 모아 두는 저장소 */
    size_t hused;

    /* 헤더에서 뽑아낸 정보 (http_finish_headers가 채움) */
    long long content_length;      /* 본문 길이, 없으면 -1 */
    int  chunked;                  /* Transfer-Encoding: chunked 이면 1 */
    int  keep_alive;               /* 응답 후 연결을 유지할지 */
} http_request_t;

/*
 * http_read_request — 소켓에서 요청 줄 + 헤더를 읽고 해석까지 한다 (본문은 읽지 않음).
 *   입력: r(연결의 수신 버퍼), req(채울 곳)
 *   결과: 0 = 성공
 *         음수(NET_EOF/NET_ERR/NET_TIMEOUT) = 응답 없이 연결을 닫아야 함 (요청을 시작도 안 하고 끊음/대기 초과)
 *         양수 = 그 HTTP 상태 코드로 오류 응답을 보내야 함 (400, 408, 414, 431, 501, 505)
 */
int http_read_request(net_reader_t *r, http_request_t *req);

/*
 * http_read_body — 헤더 다음에 오는 본문을 끝까지 읽어 req->body에 담는다.
 *   입력: r, req(헤더 해석이 끝난 상태), max(허용 크기)
 *   결과: 0 = 성공 (본문이 없으면 body=NULL, body_len=0)
 *         양수 = 오류 응답 코드: 413(너무 큼), 400(chunked 형식 오류/중간에 끊김), 408(시간 초과)
 *   - Content-Length: 그 길이만큼 정확히 읽는다
 *   - chunked: "크기(16진수)\r\n 데이터\r\n" 반복, 크기 0이 나오면 끝
 *   - "Expect: 100-continue"면 읽기 전에 "100 Continue"를 보내 클라이언트가 본문을 보내게 한다
 */
int http_read_body(net_reader_t *r, http_request_t *req, size_t max);

/* http_free_body — 본문 메모리 반납 (요청 하나 처리가 끝날 때마다 호출) */
void http_free_body(http_request_t *req);

/* 아래 세 함수는 http_read_request가 차례로 부르는 단계들 (테스트에서 따로 검사) */

/* http_parse_request_line — "GET /a HTTP/1.1" 한 줄 해석. 결과: 0 또는 상태 코드 */
int http_parse_request_line(const char *line, http_request_t *req);

/* http_parse_header_line — "Host: a" 한 줄 해석해서 추가. 결과: 0 또는 상태 코드 */
int http_parse_header_line(const char *line, http_request_t *req);

/* http_finish_headers — 헤더를 다 읽은 뒤 Host/본문 길이/keep-alive 판단. 결과: 0 또는 상태 코드 */
int http_finish_headers(http_request_t *req);

/* http_get_header — 이름으로 헤더 값 찾기 (대소문자 무시). 없으면 NULL */
const char *http_get_header(const http_request_t *req, const char *name);

/*
 * http_url_decode — "%20" 같은 퍼센트 인코딩을 원래 바이트로 바꾼다.
 *   입력: in/n(원문), out/outlen(결과 버퍼)
 *   결과: 0 성공, -1 잘못된 인코딩(%zz, 끝이 잘린 %, %00) 또는 버퍼 부족
 */
int http_url_decode(const char *in, size_t n, char *out, size_t outlen);

/* http_has_token — 쉼표 목록 값에 단어가 있는지 (대소문자 무시). "keep-alive, Upgrade"에서 "upgrade" → 1 */
int http_has_token(const char *list, const char *tok);

/* http_reason — 상태 코드의 표준 문구 (200 → "OK", 404 → "Not Found") */
const char *http_reason(int status);

#endif
