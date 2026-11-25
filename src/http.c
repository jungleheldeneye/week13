/*
 * http.c — HTTP 요청 파싱 구현 (http.h 설명 참고)
 *
 * 검사 순서:
 *   1) 요청 줄: 공백 정확히 2개로 세 부분 → 메서드(토큰/지원 여부) → 대상(형태/길이) → 버전
 *   2) 헤더 줄: "이름: 값" 형식, 이름에 공백 금지, 개수·크기 제한
 *   3) 마무리: HTTP/1.1이면 Host 필수, Content-Length 숫자 검사, chunked와 동시 사용 금지,
 *              keep-alive 여부 결정
 *   원칙: 애매하면 받아들이지 않고 400으로 거절한다 (요청 스머글링 같은 공격 방지).
 */
#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <unistd.h>

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 100: return "Continue";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 418: return "I'm a teapot";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 508: return "Loop Detected";
    /* 표에 없는 코드(CGI 스크립트가 정한 코드 등)는 분류 이름으로 대신함 */
    default:
        if (status >= 100 && status < 200) return "Informational";
        if (status < 300) return "Success";
        if (status < 400) return "Redirection";
        if (status < 500) return "Client Error";
        return "Server Error";
    }
}

/* is_tchar — HTTP "토큰"에 쓸 수 있는 글자인지 (메서드·헤더 이름 검사용, RFC 9110) */
static int is_tchar(int c)
{
    return isalnum(c) || (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int is_token(const char *s)
{
    if (*s == '\0') return 0;
    for (; *s; s++)
        if (!is_tchar((unsigned char)*s)) return 0;
    return 1;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int http_url_decode(const char *in, size_t n, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)in[i];
        if (c == '%') {
            if (i + 2 >= n) return -1;                    /* '%' 뒤에 두 글자가 없음 */
            int hi = hexval((unsigned char)in[i + 1]);
            int lo = hexval((unsigned char)in[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = hi * 16 + lo;
            if (c == 0) return -1;   /* %00: C 문자열을 중간에서 끊어 검사를 속이는 공격 차단 */
            i += 2;
        }
        if (o + 1 >= outlen) return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return 0;
}

/*
 * split_target — 대상 문자열을 경로와 쿼리로 나누고 경로를 해석한다.
 *   "/a%20b?x=1"           → path "/a b", query "x=1"
 *   "http://h:81/a?x=1"    → is_absolute, abs_host "h", abs_port 81, path "/a"
 */
static int split_target(http_request_t *req)
{
    const char *t = req->target;

    if (strncasecmp(t, "http://", 7) == 0) {          /* 절대 URL: 프록시 요청 형태 */
        const char *h = t + 7;
        const char *end = h + strcspn(h, "/?");       /* 호스트 부분의 끝 */
        const char *colon = memchr(h, ':', (size_t)(end - h));
        const char *hend = colon ? colon : end;
        size_t hl = (size_t)(hend - h);
        if (hl == 0 || hl >= sizeof(req->abs_host)) return 400;
        memcpy(req->abs_host, h, hl);
        req->abs_host[hl] = '\0';
        req->abs_port = 80;
        if (colon) {
            char *pe;
            long p = strtol(colon + 1, &pe, 10);
            if (pe != end || p < 1 || p > 65535) return 400;
            req->abs_port = (int)p;
        }
        req->is_absolute = 1;
        t = (*end == '\0') ? "/" : end;               /* "http://h" 만 오면 경로는 "/" */
        if (*t == '?') {                              /* "http://h?x" → 경로 "/" + 쿼리 */
            strcpy(req->path, "/");
            snprintf(req->query, sizeof(req->query), "%s", t + 1);
            return 0;
        }
    } else if (t[0] != '/') {
        return 400;   /* 일반 요청은 반드시 '/'로 시작 ("*", "abc" 등은 거절) */
    }

    const char *q = strchr(t, '?');
    size_t plen = q ? (size_t)(q - t) : strlen(t);
    if (http_url_decode(t, plen, req->path, sizeof(req->path)) != 0)
        return 400;
    if (q) snprintf(req->query, sizeof(req->query), "%s", q + 1);   /* 쿼리는 원문 그대로 */
    return 0;
}

int http_parse_request_line(const char *line, http_request_t *req)
{
    /* 정확히 "메서드 SP 대상 SP 버전" 이어야 함: 공백 위치 두 개를 찾는다 */
    const char *sp1 = strchr(line, ' ');
    if (!sp1) return 400;
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2 || strchr(sp2 + 1, ' ')) return 400;      /* 공백이 1개뿐이거나 3개 이상 */

    size_t mlen = (size_t)(sp1 - line);
    size_t tlen = (size_t)(sp2 - sp1 - 1);
    const char *ver = sp2 + 1;

    /* 1) 메서드 */
    if (mlen == 0 || mlen >= sizeof(req->method_str)) return mlen ? 501 : 400;
    memcpy(req->method_str, line, mlen);
    req->method_str[mlen] = '\0';
    if (!is_token(req->method_str)) return 400;        /* "G@T" 같은 깨진 메서드 */

    /* 2) 대상 */
    if (tlen == 0) return 400;
    if (tlen >= sizeof(req->target)) return 414;
    memcpy(req->target, sp1 + 1, tlen);
    req->target[tlen] = '\0';

    /* 3) 버전: "HTTP/1.0" 또는 "HTTP/1.1"만 지원. 형식은 맞지만 다른 버전이면 505 */
    if (strncmp(ver, "HTTP/", 5) != 0 || !isdigit((unsigned char)ver[5]) ||
        ver[6] != '.' || !isdigit((unsigned char)ver[7]) || ver[8] != '\0')
        return 400;
    if (ver[5] != '1' || (ver[7] != '0' && ver[7] != '1'))
        return 505;
    req->minor = ver[7] - '0';

    /* 메서드 지원 여부는 형식 검사가 끝난 뒤 판단 (형식이 틀리면 400이 먼저) */
    if      (strcmp(req->method_str, "GET") == 0)  req->method = HTTP_GET;
    else if (strcmp(req->method_str, "HEAD") == 0) req->method = HTTP_HEAD;
    else if (strcmp(req->method_str, "POST") == 0) req->method = HTTP_POST;
    else return 501;                                  /* PUT, DELETE 등: 아는 형식이지만 미지원 */

    return split_target(req);
}

/* hbuf_add — 문자열을 헤더 저장소에 복사하고 그 위치를 돌려준다. 공간이 없으면 NULL */
static const char *hbuf_add(http_request_t *req, const char *s, size_t n)
{
    if (req->hused + n + 1 > sizeof(req->hbuf)) return NULL;
    char *dst = req->hbuf + req->hused;
    memcpy(dst, s, n);
    dst[n] = '\0';
    req->hused += n + 1;
    return dst;
}

int http_parse_header_line(const char *line, http_request_t *req)
{
    if (line[0] == ' ' || line[0] == '\t') return 400;   /* 옛 "줄 이어쓰기" 문법: 금지 */

    const char *colon = strchr(line, ':');
    if (!colon || colon == line) return 400;

    /* 이름: 토큰 글자만, "Host :"처럼 콜론 앞 공백 금지 */
    size_t nlen = (size_t)(colon - line);
    for (size_t i = 0; i < nlen; i++)
        if (!is_tchar((unsigned char)line[i])) return 400;

    /* 값: 앞뒤 공백/탭 제거 */
    const char *v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    size_t vlen = strlen(v);
    while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) vlen--;

    if (req->nheaders >= HTTP_MAX_HEADERS) return 431;
    const char *n = hbuf_add(req, line, nlen);
    const char *val = n ? hbuf_add(req, v, vlen) : NULL;
    if (!val) return 431;

    req->headers[req->nheaders].name = n;
    req->headers[req->nheaders].value = val;
    req->nheaders++;
    return 0;
}

const char *http_get_header(const http_request_t *req, const char *name)
{
    for (int i = 0; i < req->nheaders; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)   /* "host" == "Host" */
            return req->headers[i].value;
    return NULL;
}

/* count_header — 같은 이름의 헤더가 몇 번 나왔는지 (중복 검사용) */
static int count_header(const http_request_t *req, const char *name)
{
    int c = 0;
    for (int i = 0; i < req->nheaders; i++)
        if (strcasecmp(req->headers[i].name, name) == 0) c++;
    return c;
}

/* has_token — 쉼표 목록 값에 특정 단어가 있는지 ("keep-alive, Upgrade"에서 "upgrade") */
int http_has_token(const char *list, const char *tok)
{
    size_t tl = strlen(tok);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        size_t n = strcspn(p, ",");
        size_t m = n;
        while (m > 0 && (p[m - 1] == ' ' || p[m - 1] == '\t')) m--;
        if (m == tl && strncasecmp(p, tok, tl) == 0) return 1;
        p += n;
    }
    return 0;
}

int http_finish_headers(http_request_t *req)
{
    /* Host: HTTP/1.1에서는 필수, 두 개 이상이면 어느 쪽이 맞는지 모르므로 거절 */
    int hosts = count_header(req, "Host");
    if (hosts > 1 || (req->minor == 1 && hosts == 0)) return 400;

    /* 본문 길이 */
    req->content_length = -1;
    req->chunked = 0;
    const char *te = http_get_header(req, "Transfer-Encoding");
    const char *cl = http_get_header(req, "Content-Length");

    if (te) {
        if (strcasecmp(te, "chunked") != 0) return 501;   /* gzip 등 다른 인코딩은 미지원 */
        req->chunked = 1;
    }
    if (cl) {
        /* 둘 다 있으면 서버마다 해석이 달라질 수 있음 → 요청 스머글링 위험이라 거절 */
        if (te || count_header(req, "Content-Length") > 1) return 400;
        if (!isdigit((unsigned char)cl[0])) return 400;   /* "-5", "+5", " " 거절 */
        char *end;
        errno = 0;
        long long v = strtoll(cl, &end, 10);
        if (errno != 0 || *end != '\0') return 400;       /* "10abc", 너무 큰 수 거절 */
        req->content_length = v;
    }

    /* keep-alive: 1.1은 기본 유지(close라고 하면 끊음), 1.0은 기본 끊음(keep-alive라고 하면 유지) */
    const char *conn = http_get_header(req, "Connection");
    if (req->minor == 1)
        req->keep_alive = !(conn && http_has_token(conn, "close"));
    else
        req->keep_alive = (conn && http_has_token(conn, "keep-alive"));
    return 0;
}

int http_read_request(net_reader_t *r, http_request_t *req)
{
    char line[HTTP_MAX_TARGET + 64];
    ssize_t n;

    memset(req, 0, offsetof(http_request_t, hbuf));   /* 큰 hbuf는 지울 필요 없음 */
    req->hused = 0;

    /* 요청 줄. 앞의 빈 줄 몇 개는 무시 (RFC 권장: 이전 요청 뒤에 붙은 CRLF 허용) */
    int skipped = 0;
    do {
        n = net_read_line(r, line, sizeof(line));
    } while (n == 0 && ++skipped < 4);
    if (n == NET_TOOLONG) return 414;
    if (n < 0) return (int)n;                 /* EOF/ERR: 조용히 닫기 */
    if (n == 0) return 400;

    int st = http_parse_request_line(line, req);
    if (st) return st;

    /* 헤더: 빈 줄이 나올 때까지 */
    for (;;) {
        n = net_read_line(r, line, sizeof(line));
        if (n == 0) break;
        if (n == NET_TOOLONG) return 431;
        if (n == NET_EOF) return 400;         /* 헤더 중간에 끊김 = 불완전한 요청 */
        if (n == NET_TIMEOUT) return 408;     /* 헤더를 보내다 멈춤 (느린 공격 포함) */
        if (n < 0) return (int)n;
        st = http_parse_header_line(line, req);
        if (st) return st;
    }
    return http_finish_headers(req);
}

/* body_status — 본문 읽기 중 net 오류를 HTTP 상태로 (끊김/형식 오류 400, 시간 초과 408) */
static int body_status(ssize_t n)
{
    return n == NET_TIMEOUT ? 408 : 400;
}

/* read_chunked — chunked 본문을 풀어서 하나의 연속된 본문으로 만든다 */
static int read_chunked(net_reader_t *r, http_request_t *req, size_t max)
{
    char line[256];
    size_t cap = 0;

    for (;;) {
        /* 1) 조각 크기 줄: "1a3f" 또는 "1a3f;확장=값" (확장은 무시) */
        ssize_t n = net_read_line(r, line, sizeof(line));
        if (n == NET_TOOLONG) return 400;
        if (n < 0) return body_status(n);
        line[strcspn(line, "; \t")] = '\0';
        if (line[0] == '\0' || !isxdigit((unsigned char)line[0])) return 400;
        char *end;
        errno = 0;
        unsigned long long sz = strtoull(line, &end, 16);
        if (errno != 0 || *end != '\0') return 400;      /* "1g", 너무 큰 수 */

        /* 2) 크기 0 = 마지막 조각. 뒤따르는 트레일러 헤더는 빈 줄까지 읽고 버린다 */
        if (sz == 0) {
            for (int i = 0; i < HTTP_MAX_HEADERS; i++) {
                n = net_read_line(r, line, sizeof(line));
                if (n == 0) return 0;
                if (n < 0 && n != NET_TOOLONG) return body_status(n);
            }
            return 400;
        }

        /* 3) 상한 검사 후 버퍼를 늘려 조각 데이터를 정확히 sz바이트 읽는다 */
        if (sz > max - req->body_len) return 413;
        size_t need = req->body_len + (size_t)sz + 1;
        if (need > cap) {
            size_t ncap = cap ? cap : 4096;
            while (ncap < need) ncap *= 2;                  /* 2배씩 늘려 realloc 횟수를 줄임 */
            if (ncap > max + 1) ncap = max + 1;
            char *nb = realloc(req->body, ncap);
            if (!nb) return 500;
            req->body = nb;
            cap = ncap;
        }
        n = net_read_n(r, req->body + req->body_len, (size_t)sz);
        if (n < 0) return body_status(n);
        req->body_len += (size_t)sz;
        req->body[req->body_len] = '\0';

        /* 4) 데이터 바로 뒤에는 반드시 빈 줄(CRLF)이 와야 함 */
        n = net_read_line(r, line, sizeof(line));
        if (n != 0) return n < 0 && n != NET_TOOLONG ? body_status(n) : 400;
    }
}

int http_read_body(net_reader_t *r, http_request_t *req, size_t max)
{
    req->body = NULL;
    req->body_len = 0;
    if (!req->chunked && req->content_length <= 0) return 0;   /* 본문 없음 */

    /* 크기를 미리 알면 읽기 전에 거절 (8MB를 다 받고 나서 거절하는 낭비 방지) */
    if (!req->chunked && (unsigned long long)req->content_length > max) return 413;

    /* 클라이언트가 "보내도 될까요?"(Expect: 100-continue)라고 물었으면 먼저 허락한다.
     * curl은 1MB 넘는 업로드에서 이렇게 묻고, 답이 없으면 1초를 기다린다. */
    const char *expect = http_get_header(req, "Expect");
    if (expect && strcasecmp(expect, "100-continue") == 0 && req->minor == 1) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (net_write_all(r->fd, cont, sizeof(cont) - 1) != 0) return 400;
    }

    if (req->chunked) {
        int st = read_chunked(r, req, max);
        if (st != 0) http_free_body(req);
        return st;
    }

    req->body = malloc((size_t)req->content_length + 1);
    if (!req->body) return 500;
    ssize_t n = net_read_n(r, req->body, (size_t)req->content_length);
    if (n < 0) { http_free_body(req); return body_status(n); }
    req->body_len = (size_t)req->content_length;
    req->body[req->body_len] = '\0';
    return 0;
}

void http_free_body(http_request_t *req)
{
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}
