/*
 * proxy.c — Reverse/Forward Proxy 구현 (proxy.h 설명 참고)
 *
 * 중계 흐름 (relay):
 *   1) 백엔드에 접속 (Reverse는 Round-Robin + 실패 시 다음 백엔드로)
 *   2) 요청 재구성: 요청 줄 + 헤더(홉 단위 헤더 제거, X-Forwarded-For/Via 추가) + 본문
 *   3) 응답 상태 줄·헤더 읽기 (시간 초과 504, 형식 오류 502)
 *   4) 응답 헤더를 다듬어 클라이언트에 보내고, 본문을 길이 방식에 맞게 그대로 흘려보냄
 *
 * 홉 단위(hop-by-hop) 헤더: Connection, Keep-Alive, Transfer-Encoding 등은
 *   "바로 옆 상대와의 연결"에만 의미가 있어서, 프록시는 전달하지 않고 자기 연결에 맞게 새로 쓴다.
 */
#include "proxy.h"
#include "net.h"
#include "response.h"
#include "cache.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char        host[256];
    int         port;
    atomic_long down_until;     /* 이 시각 전까지는 "죽은 것으로 보고" 건너뜀 */
} backend_t;

typedef struct {
    int          is_host;       /* 1: Host 헤더 규칙, 0: 경로 접두어 규칙 */
    char         match[256];
    size_t       mlen;
    int          strip;         /* 1: 접두어를 떼고 전달 */
    backend_t    be[PROXY_MAX_BACKENDS];
    int          nb;
    atomic_uint  rr;            /* Round-Robin 순번 (여러 워커가 동시에 증가 → atomic) */
} route_t;

typedef struct {
    route_t r[PROXY_MAX_ROUTES];
    int     n;
} table_t;

/* 규칙 표는 공유 메모리(MAP_SHARED)에 둔다: -m fork에서 자식 프로세스들이 Round-Robin 순번을
 * 함께 써야 번갈아 가기 때문 (일반 메모리면 자식마다 복사본이 생겨 늘 같은 백엔드로 감) */
static table_t *g_tab;

/* ---------------- 설정 파일 ---------------- */

/* parse_backend — "127.0.0.1:8001" 또는 "[::1]:8001" → host, port. 결과 0/-1 */
static int parse_backend(const char *s, backend_t *b)
{
    const char *colon;
    size_t hl;
    if (s[0] == '[') {                                   /* IPv6 */
        const char *rb = strchr(s, ']');
        if (!rb || rb[1] != ':') return -1;
        hl = (size_t)(rb - s - 1);
        if (hl == 0 || hl >= sizeof(b->host)) return -1;
        memcpy(b->host, s + 1, hl);
        colon = rb + 1;
    } else {
        colon = strrchr(s, ':');
        if (!colon) return -1;
        hl = (size_t)(colon - s);
        if (hl == 0 || hl >= sizeof(b->host)) return -1;
        memcpy(b->host, s, hl);
    }
    b->host[hl] = '\0';
    char *end;
    long p = strtol(colon + 1, &end, 10);
    if (*end != '\0' || p < 1 || p > 65535) return -1;
    b->port = (int)p;
    atomic_init(&b->down_until, 0);
    return 0;
}

int proxy_init(const char *path, char *err, int errlen)
{
    g_tab = mmap(NULL, sizeof(table_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (g_tab == MAP_FAILED) { g_tab = NULL; snprintf(err, errlen, "공유 메모리 할당 실패"); return -1; }
    memset(g_tab, 0, sizeof(*g_tab));
    if (!path || !path[0]) return 0;

    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "설정 파일을 열 수 없습니다: %s", path); return -1; }

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "#\r\n")] = '\0';            /* 주석과 줄바꿈 제거 */
        char *save, *kind = strtok_r(line, " \t", &save);
        if (!kind) continue;                             /* 빈 줄 */

        if (g_tab->n == PROXY_MAX_ROUTES) { snprintf(err, errlen, "%d번째 줄: 규칙이 너무 많습니다(최대 %d)", lineno, PROXY_MAX_ROUTES); goto fail; }
        route_t *rt = &g_tab->r[g_tab->n];
        if (strcmp(kind, "route") == 0) rt->is_host = 0;
        else if (strcmp(kind, "host") == 0) rt->is_host = 1;
        else { snprintf(err, errlen, "%d번째 줄: route 또는 host로 시작해야 합니다", lineno); goto fail; }

        char *m = strtok_r(NULL, " \t", &save);
        if (!m || strlen(m) >= sizeof(rt->match) || (!rt->is_host && m[0] != '/')) {
            snprintf(err, errlen, "%d번째 줄: %s", lineno, rt->is_host ? "호스트 이름이 필요합니다" : "경로는 /로 시작해야 합니다");
            goto fail;
        }
        strcpy(rt->match, m);
        rt->mlen = strlen(m);

        char *tok;
        while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
            if (strcmp(tok, "strip") == 0) { rt->strip = 1; continue; }
            if (rt->nb == PROXY_MAX_BACKENDS) { snprintf(err, errlen, "%d번째 줄: 백엔드가 너무 많습니다(최대 %d)", lineno, PROXY_MAX_BACKENDS); goto fail; }
            if (parse_backend(tok, &rt->be[rt->nb]) != 0) { snprintf(err, errlen, "%d번째 줄: 백엔드 주소가 틀렸습니다: %s (예: 127.0.0.1:8001)", lineno, tok); goto fail; }
            rt->nb++;
        }
        if (rt->nb == 0) { snprintf(err, errlen, "%d번째 줄: 백엔드가 하나 이상 필요합니다", lineno); goto fail; }
        atomic_init(&rt->rr, 0);
        g_tab->n++;
    }
    fclose(f);
    return 0;
fail:
    fclose(f);
    return -1;
}

int proxy_route_count(void) { return g_tab ? g_tab->n : 0; }

void proxy_describe(int i, char *out, size_t outlen)
{
    const route_t *rt = &g_tab->r[i];
    int n = snprintf(out, outlen, "%s %s%s →", rt->is_host ? "Host" : "경로", rt->match,
                     rt->strip ? " (접두어 제거)" : "");
    for (int b = 0; b < rt->nb && n > 0 && (size_t)n < outlen; b++)
        n += snprintf(out + n, outlen - (size_t)n, " %s:%d", rt->be[b].host, rt->be[b].port);
}

int proxy_match(const http_request_t *req)
{
    if (!g_tab) return -1;
    /* 1) Host 헤더 규칙: "api.local:8080" → 포트를 떼고 비교 */
    const char *host = http_get_header(req, "Host");
    if (host) {
        char h[256];
        size_t hl = host[0] == '[' ? strcspn(host, "]") + 1 : strcspn(host, ":");
        if (hl < sizeof(h)) {
            memcpy(h, host, hl);
            h[hl] = '\0';
            for (int i = 0; i < g_tab->n; i++)
                if (g_tab->r[i].is_host && strcasecmp(g_tab->r[i].match, h) == 0) return i;
        }
    }
    /* 2) 경로 접두어 규칙: 가장 길게 맞는 규칙 우선 ("/api/v2/"가 "/api/"보다 우선) */
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < g_tab->n; i++) {
        const route_t *rt = &g_tab->r[i];
        if (rt->is_host) continue;
        int ok = strncmp(req->path, rt->match, rt->mlen) == 0;
        /* "/api/" 규칙은 "/api" 요청도 받는다 */
        if (!ok && rt->match[rt->mlen - 1] == '/' && strlen(req->path) == rt->mlen - 1 &&
            strncmp(req->path, rt->match, rt->mlen - 1) == 0) ok = 1;
        if (ok && rt->mlen > best_len) { best = i; best_len = rt->mlen; }
    }
    return best;
}

/* ---------------- 중계 ---------------- */

/* is_hop — 프록시가 전달하면 안 되는(자기가 새로 쓰는) 헤더인지 */
static int is_hop(const char *name, const char *conn_tokens)
{
    static const char *hop[] = {
        "Connection", "Keep-Alive", "Proxy-Connection", "Proxy-Authenticate", "Proxy-Authorization",
        "TE", "Trailer", "Transfer-Encoding", "Upgrade", "Content-Length", "Expect", NULL };
    for (int i = 0; hop[i]; i++)
        if (strcasecmp(name, hop[i]) == 0) return 1;
    /* "Connection: X-Foo" 처럼 Connection에 이름이 적힌 헤더도 홉 단위 */
    return conn_tokens && http_has_token(conn_tokens, name);
}

/* append_fmt — 고정 버퍼 뒤에 이어 쓰기. 넘치면 -1 */
static int append_fmt(char *buf, size_t cap, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int append_fmt(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) return -1;
    *len += (size_t)n;
    return 0;
}

/* proxy_fail — 중계 실패를 클라이언트에 알린다 (502/504/508) */
static int proxy_fail(int fd, int status, const char *why, int *keep_out, long long *sent)
{
    *keep_out = 0;
    long long n = resp_error(fd, status, 0, 0, NULL);
    *sent = n > 0 ? n : 0;
    fprintf(stderr, "[프록시] %d: %s\n", status, why);
    return n < 0 ? -1 : status;
}

enum { BODY_NONE, BODY_LENGTH, BODY_CHUNKED, BODY_UNTIL_CLOSE };

/* capture_t — 중계하면서 본문을 모아 두는 버퍼 (캐시 저장용). 상한을 넘으면 포기(over=1) */
typedef struct { char *p; size_t len, cap; int over; } capture_t;

static void capture_add(capture_t *c, const char *data, size_t n)
{
    if (!c || c->over) return;
    if (c->len + n > CACHE_MAX_OBJECT) { c->over = 1; return; }      /* 너무 큼 → 저장 안 함 */
    if (c->len + n > c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 16384;
        while (nc < c->len + n) nc *= 2;
        char *np = realloc(c->p, nc);
        if (!np) { c->over = 1; return; }
        c->p = np;
        c->cap = nc;
    }
    memcpy(c->p + c->len, data, n);
    c->len += n;
}

/* copy_n — 백엔드에서 정확히 n바이트를 받아 클라이언트로 보낸다 (64KB씩). 결과 0/-1
 *   cap: NULL이 아니면 보낸 내용을 여기에도 모음 (캐시에 저장하려고) */
static int copy_n(net_reader_t *up, int fd, unsigned long long n, long long *sent, capture_t *cap)
{
    static _Thread_local char buf[65536];
    while (n > 0) {
        size_t want = n < sizeof(buf) ? (size_t)n : sizeof(buf);
        ssize_t k = net_read_some(up, buf, want);
        if (k <= 0) return -1;
        if (net_write_all(fd, buf, (size_t)k) != 0) return -1;
        capture_add(cap, buf, (size_t)k);
        n -= (unsigned long long)k;
        *sent += k;
    }
    return 0;
}

/* relay_chunked — chunked 본문을 조각 단위로 중계. passthrough=0이면 조각 표시를 벗기고 데이터만 보냄 */
static int relay_chunked(net_reader_t *up, int fd, int passthrough, long long *sent, capture_t *cap)
{
    char line[256];
    for (;;) {
        ssize_t n = net_read_line(up, line, sizeof(line));
        if (n < 0) return -1;
        line[strcspn(line, "; \t")] = '\0';
        char *end;
        unsigned long long sz = strtoull(line, &end, 16);
        if (line[0] == '\0' || *end != '\0') return -1;
        if (passthrough) {
            char hdr[32];
            int hl = snprintf(hdr, sizeof(hdr), "%llx\r\n", sz);
            if (net_write_all(fd, hdr, (size_t)hl) != 0) return -1;
        }
        if (sz == 0) {                                   /* 마지막 조각: 트레일러는 버리고 끝 표시 */
            do { n = net_read_line(up, line, sizeof(line)); } while (n > 0);
            if (n < 0) return -1;
            return passthrough ? net_write_all(fd, "\r\n", 2) : 0;
        }
        if (copy_n(up, fd, sz, sent, cap) != 0) return -1;
        if (net_read_line(up, line, sizeof(line)) != 0) return -1;   /* 조각 뒤 CRLF */
        if (passthrough && net_write_all(fd, "\r\n", 2) != 0) return -1;
    }
}

/*
 * relay — 접속된 백엔드 소켓(up)으로 요청을 보내고 응답을 클라이언트(fd)에 중계한다.
 *   target: 백엔드에 보낼 요청 대상("/a?x=1"), host_hdr: 보낼 Host 값
 *   cache_key: Forward Proxy 캐시 키(URL). NULL이면 캐시 안 씀 (Reverse Proxy)
 */
static int relay(int fd, int upfd, const http_request_t *req, const char *target, const char *host_hdr,
                 const char *client_ip, int port, int keep, int *keep_out, long long *sent,
                 const char *cache_key)
{
    static _Thread_local char head[32768];
    static _Thread_local net_reader_t up;
    static _Thread_local http_request_t rh;      /* 응답 헤더를 담는 그릇 (헤더 해석 함수를 재사용) */
    size_t hl = 0;
    const char *conn = http_get_header(req, "Connection");

    /* 1) 요청 재구성 */
    int bad = append_fmt(head, sizeof(head), &hl, "%s %s HTTP/1.1\r\n", req->method_str, target);
    for (int i = 0; i < req->nheaders && !bad; i++) {
        const char *n = req->headers[i].name;
        if (is_hop(n, conn) || strcasecmp(n, "Host") == 0 || strcasecmp(n, "X-Forwarded-For") == 0 ||
            strcasecmp(n, "Via") == 0)
            continue;
        bad = append_fmt(head, sizeof(head), &hl, "%s: %s\r\n", n, req->headers[i].value);
    }
    const char *xff = http_get_header(req, "X-Forwarded-For");
    const char *via = http_get_header(req, "Via");
    const char *orig_host = http_get_header(req, "Host");
    if (!bad) bad = append_fmt(head, sizeof(head), &hl,
        "Host: %s\r\n"
        "X-Forwarded-For: %s%s%s\r\n"            /* 원래 클라이언트 IP (백엔드는 우리 IP만 보이므로) */
        "X-Forwarded-Proto: http\r\n"
        "%s%s%s"
        "Via: %s%s1.1 cweb-%d\r\n"               /* 거쳐 간 프록시 기록 (루프 감지에도 사용) */
        "Connection: close\r\n",                 /* 백엔드 연결은 요청 하나 쓰고 닫음 (단순·안전) */
        host_hdr, xff ? xff : "", xff ? ", " : "", client_ip,
        orig_host ? "X-Forwarded-Host: " : "", orig_host ? orig_host : "", orig_host ? "\r\n" : "",
        via ? via : "", via ? ", " : "", port);
    if (!bad && (req->body_len > 0 || req->method == HTTP_POST))
        bad = append_fmt(head, sizeof(head), &hl, "Content-Length: %zu\r\n", req->body_len);
    if (!bad) bad = append_fmt(head, sizeof(head), &hl, "\r\n");
    if (bad) return proxy_fail(fd, 502, "요청 헤더가 너무 큼", keep_out, sent);

    /* 2) 요청 헤더 + 본문을 한 번에 전송 */
    net_set_timeout(upfd, PROXY_RESPONSE_TIMEOUT);
    struct iovec iov[2] = { { head, hl }, { req->body, req->body_len } };
    if (net_writev_all(upfd, iov, req->body_len ? 2 : 1) != 0)
        return proxy_fail(fd, 502, "백엔드로 요청 전송 실패", keep_out, sent);

    /* 3) 응답 상태 줄 (100 Continue 같은 1xx 중간 응답은 건너뜀) */
    net_reader_init(&up, upfd);
    char line[8192];
    int status = 0;
    char reason[128] = "";
    do {
        ssize_t n = net_read_line(&up, line, sizeof(line));
        if (n == NET_TIMEOUT) return proxy_fail(fd, 504, "백엔드 응답 시간 초과", keep_out, sent);
        if (n <= 0) return proxy_fail(fd, 502, "백엔드가 응답 없이 끊음", keep_out, sent);
        if (strncmp(line, "HTTP/1.", 7) != 0 || !isdigit((unsigned char)line[9]) ||
            sscanf(line + 9, "%3d", &status) != 1 || status < 100 || status > 599)
            return proxy_fail(fd, 502, "백엔드 응답 형식 오류", keep_out, sent);
        snprintf(reason, sizeof(reason), "%.120s", line[12] == ' ' ? line + 13 : http_reason(status));
        if (status >= 100 && status < 200) {             /* 중간 응답: 헤더 끝까지 버리고 다음 줄 */
            if (status == 101) return proxy_fail(fd, 502, "프로토콜 전환(101)은 지원하지 않음", keep_out, sent);
            while ((n = net_read_line(&up, line, sizeof(line))) > 0) ;
            if (n < 0) return proxy_fail(fd, 502, "백엔드 응답 형식 오류", keep_out, sent);
        }
    } while (status < 200);

    /* 4) 응답 헤더 */
    rh.nheaders = 0;
    rh.hused = 0;
    for (;;) {
        ssize_t n = net_read_line(&up, line, sizeof(line));
        if (n == 0) break;
        if (n == NET_TIMEOUT) return proxy_fail(fd, 504, "백엔드 응답 시간 초과", keep_out, sent);
        if (n < 0 || http_parse_header_line(line, &rh) != 0)
            return proxy_fail(fd, 502, "백엔드 응답 헤더 오류", keep_out, sent);
    }

    /* 5) 본문 길이 방식 결정 */
    const char *te = http_get_header(&rh, "Transfer-Encoding");
    const char *cl = http_get_header(&rh, "Content-Length");
    int mode;
    unsigned long long clen = 0;
    if (req->method == HTTP_HEAD || status == 204 || status == 304) mode = BODY_NONE;
    else if (te && http_has_token(te, "chunked")) mode = BODY_CHUNKED;
    else if (cl) {
        char *end;
        clen = strtoull(cl, &end, 10);
        if (!isdigit((unsigned char)cl[0]) || *end != '\0')
            return proxy_fail(fd, 502, "백엔드 Content-Length 오류", keep_out, sent);
        mode = BODY_LENGTH;
    } else mode = BODY_UNTIL_CLOSE;                       /* 길이 표시 없음: 백엔드가 닫을 때까지 */

    /* 클라이언트 쪽 연결 유지: 응답 끝을 클라이언트가 알 수 있을 때만 */
    int pass_chunked = (mode == BODY_CHUNKED && req->minor == 1);
    int k = keep && (mode == BODY_NONE || mode == BODY_LENGTH || pass_chunked);

    /* 6) 응답 헤더를 다듬어 클라이언트에 보냄 */
    hl = 0;
    const char *rconn = http_get_header(&rh, "Connection");
    bad = append_fmt(head, sizeof(head), &hl, "HTTP/1.1 %d %s\r\n", status, reason);
    for (int i = 0; i < rh.nheaders && !bad; i++) {
        const char *n = rh.headers[i].name;
        if (is_hop(n, rconn) || strcasecmp(n, "Via") == 0) continue;
        bad = append_fmt(head, sizeof(head), &hl, "%s: %s\r\n", n, rh.headers[i].value);
    }
    const char *rvia = http_get_header(&rh, "Via");
    if (!bad) bad = append_fmt(head, sizeof(head), &hl, "Via: %s%s1.1 cweb-%d\r\n",
                               rvia ? rvia : "", rvia ? ", " : "", port);
    if (!bad && mode == BODY_LENGTH) bad = append_fmt(head, sizeof(head), &hl, "Content-Length: %llu\r\n", clen);
    if (!bad && mode == BODY_NONE && cl) bad = append_fmt(head, sizeof(head), &hl, "Content-Length: %s\r\n", cl);

    /* 캐시 저장 여부: GET의 200 응답 + 길이를 아는 1MB 이하 본문 + 헤더가 저장을 허락할 때 */
    int ttl = -1;
    if (cache_key && req->method == HTTP_GET && status == 200 &&
        (mode == BODY_CHUNKED || (mode == BODY_LENGTH && clen <= CACHE_MAX_OBJECT)) &&
        !http_get_header(req, "Authorization"))
        ttl = cache_ttl(http_get_header(&rh, "Cache-Control"), http_get_header(&rh, "Set-Cookie") != NULL,
                        http_get_header(&rh, "Vary") != NULL);
    size_t store_len = hl;                       /* 여기까지(상태 줄 + 헤더)를 캐시에 저장 */
    if (!bad && cache_key) bad = append_fmt(head, sizeof(head), &hl, "X-Cache: MISS\r\n");
    if (!bad && pass_chunked) bad = append_fmt(head, sizeof(head), &hl, "Transfer-Encoding: chunked\r\n");
    if (!bad) bad = append_fmt(head, sizeof(head), &hl, k ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
    if (bad) return proxy_fail(fd, 502, "백엔드 응답 헤더가 너무 큼", keep_out, sent);
    if (net_write_all(fd, head, hl) != 0) { *keep_out = 0; return -1; }

    /* 7) 본문 중계: 여기부터는 이미 헤더를 보냈으므로 실패하면 연결을 끊는 수밖에 없음 */
    int rc = 0;
    *sent = 0;
    capture_t cap = {0};
    capture_t *capp = ttl > 0 ? &cap : NULL;
    if (mode == BODY_LENGTH) rc = copy_n(&up, fd, clen, sent, capp);
    else if (mode == BODY_CHUNKED) rc = relay_chunked(&up, fd, pass_chunked, sent, capp);
    else if (mode == BODY_UNTIL_CLOSE) {
        static _Thread_local char buf[65536];
        ssize_t n;
        while ((n = net_read_some(&up, buf, sizeof(buf))) > 0) {
            if (net_write_all(fd, buf, (size_t)n) != 0) { rc = -1; break; }
            *sent += n;
        }
        if (n < 0 && n != NET_EOF) rc = -1;
    }
    /* 본문을 끝까지 잘 받았으면 캐시에 저장. chunked였으면 조각 표시를 벗긴 데이터 + Content-Length로 저장
     * (head의 store_len 앞부분에는 Transfer-Encoding/Connection 줄이 없음) */
    if (rc == 0 && capp && !cap.over) {
        if (mode == BODY_CHUNKED) {
            char clh[48];
            int cl2 = snprintf(clh, sizeof(clh), "Content-Length: %zu\r\n", cap.len);
            if (store_len + (size_t)cl2 <= sizeof(head)) {
                memmove(head + store_len, clh, (size_t)cl2);     /* head 뒷부분(MISS 등)은 이미 보냈으니 덮어써도 됨 */
                cache_put(cache_key, head, store_len + (size_t)cl2, cap.p ? cap.p : "", cap.len, ttl);
            }
        } else {
            cache_put(cache_key, head, store_len, cap.p ? cap.p : "", cap.len, ttl);
        }
    }
    free(cap.p);
    *keep_out = rc == 0 && k;
    return rc == 0 ? status : -1;
}

/* loop_detected — 이 요청이 이미 우리 서버를 거쳐 왔는지 (Via에 "cweb-포트"가 있는지) */
static int loop_detected(const http_request_t *req, int port)
{
    const char *via = http_get_header(req, "Via");
    char me[32];
    snprintf(me, sizeof(me), "cweb-%d", port);
    return via && strstr(via, me) != NULL;
}

/* origin_target — 백엔드에 보낼 요청 대상 만들기: 절대 URL이면 "http://host" 부분을 떼어 낸다 */
static void origin_target(const http_request_t *req, char *out, size_t outlen)
{
    const char *t = req->target;
    if (req->is_absolute) {
        t += 7;                                   /* "http://" */
        t += strcspn(t, "/?");
    }
    snprintf(out, outlen, "%s%s", (*t == '/') ? "" : "/", t);
}

int proxy_reverse(int fd, int route, const http_request_t *req, const char *client_ip, int port,
                  int keep, int *keep_out, long long *sent)
{
    route_t *rt = &g_tab->r[route];
    *sent = 0;
    if (loop_detected(req, port)) return proxy_fail(fd, 508, "프록시 루프 감지", keep_out, sent);

    /* 보낼 대상: strip이면 "/api/users?x=1" → "/users?x=1" */
    char target[HTTP_MAX_TARGET + 8];
    origin_target(req, target, sizeof(target));
    if (rt->strip && !rt->is_host && strncmp(target, rt->match, rt->mlen) == 0) {
        const char *rest = target + rt->mlen;
        if (rt->match[rt->mlen - 1] == '/') rest--;      /* "/api/" 를 떼도 '/'는 남김 */
        memmove(target, rest, strlen(rest) + 1);
        if (target[0] != '/') {                          /* "/api" → "" 또는 "?x" → "/" 붙이기 */
            memmove(target + 1, target, strlen(target) + 1);
            target[0] = '/';
        }
    }

    /* Round-Robin: 다음 순번부터 돌며 살아 있는 백엔드에 접속 (실패하면 잠시 '다운' 표시 후 다음으로) */
    unsigned start = atomic_fetch_add(&rt->rr, 1);
    time_t now = time(NULL);
    for (int pass = 0; pass < 2; pass++) {               /* 1회차: 다운 표시된 것 건너뜀, 2회차: 전부 시도 */
        for (int i = 0; i < rt->nb; i++) {
            backend_t *b = &rt->be[(start + (unsigned)i) % (unsigned)rt->nb];
            if (pass == 0 && atomic_load(&b->down_until) > now) continue;
            char err[256];
            int up = net_connect(b->host, b->port, PROXY_CONNECT_TIMEOUT, err, sizeof(err));
            if (up < 0) {
                atomic_store(&b->down_until, (long)(now + PROXY_DOWN_SECONDS));
                fprintf(stderr, "[프록시] %s → 다음 백엔드 시도\n", err);
                continue;
            }
            atomic_store(&b->down_until, 0);
            const char *host = http_get_header(req, "Host");
            char hostbuf[300];
            if (!host) { snprintf(hostbuf, sizeof(hostbuf), "%s:%d", b->host, b->port); host = hostbuf; }
            int st = relay(fd, up, req, target, host, client_ip, port, keep, keep_out, sent, NULL);
            close(up);
            return st;
        }
        if (rt->nb == 1) break;
    }
    return proxy_fail(fd, 502, "모든 백엔드 접속 실패", keep_out, sent);
}

int proxy_forward(int fd, const http_request_t *req, const char *client_ip, int port,
                  int keep, int *keep_out, long long *sent)
{
    *sent = 0;
    if (loop_detected(req, port)) return proxy_fail(fd, 508, "프록시 루프 감지", keep_out, sent);

    char target[HTTP_MAX_TARGET + 8];
    origin_target(req, target, sizeof(target));

    /* 캐시 확인: 같은 URL의 저장된 응답이 아직 유효하면 원 서버에 가지 않고 바로 응답 */
    char key[HTTP_MAX_TARGET + 300];
    char lhost[256];
    size_t i;
    for (i = 0; req->abs_host[i] && i < sizeof(lhost) - 1; i++) lhost[i] = (char)tolower((unsigned char)req->abs_host[i]);
    lhost[i] = '\0';                               /* 호스트 이름은 대소문자 구분 없음 → 소문자로 통일 */
    snprintf(key, sizeof(key), "http://%s:%d%s", lhost, req->abs_port, target);
    int use_cache = cache_request_ok(req);
    cache_hit_t hit;
    if (use_cache && cache_get(key, &hit)) {
        char extra[128];
        int el = snprintf(extra, sizeof(extra), "Age: %ld\r\nX-Cache: HIT\r\n%s", hit.age,
                          keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
        int head_only = req->method == HTTP_HEAD;
        struct iovec iov[3] = { { hit.head, hit.head_len }, { extra, (size_t)el }, { hit.body, hit.body_len } };
        int rc = net_writev_all(fd, iov, head_only ? 2 : 3);
        *sent = (rc == 0 && !head_only) ? (long long)hit.body_len : 0;
        *keep_out = rc == 0 && keep;
        cache_hit_free(&hit);
        return rc == 0 ? 200 : -1;
    }

    char err[256];
    int up = net_connect(req->abs_host, req->abs_port, PROXY_CONNECT_TIMEOUT, err, sizeof(err));
    if (up < 0) return proxy_fail(fd, errno == ETIMEDOUT ? 504 : 502, err, keep_out, sent);

    /* Host는 요청 URL의 호스트가 기준 (RFC 9112: 절대 URL이 오면 Host 헤더보다 우선) */
    char host[300];
    if (req->abs_port == 80) snprintf(host, sizeof(host), "%s", req->abs_host);
    else snprintf(host, sizeof(host), "%s:%d", req->abs_host, req->abs_port);
    /* no-cache 요청이어도 받은 응답은 저장해서 다음 요청에 씀 (요청이 GET일 때만, relay 안에서 판단) */
    int st = relay(fd, up, req, target, host, client_ip, port, keep, keep_out, sent,
                   (req->method == HTTP_GET && !http_get_header(req, "Authorization")) ? key : NULL);
    close(up);
    return st;
}
