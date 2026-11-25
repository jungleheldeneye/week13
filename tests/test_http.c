/*
 * test_http.c — HTTP 요청 파싱 테스트
 *   정상 요청, URL 디코딩, 쿼리 분리, 절대 URL(프록시용), 그리고
 *   명세 "잘못된 HTTP 요청(불완전한 헤더, 비정상 메서드, 잘못된 버전)"에 해당하는 거절 사례를 확인한다.
 */
#include "test.h"
#include "../src/http.h"

#include <sys/socket.h>
#include <unistd.h>

static http_request_t req;   /* 크기가 커서 전역에 둠 */

/* feed — 원문 요청을 소켓 한쪽에 쓰고 다른 쪽에서 http_read_request로 읽은 결과를 돌려준다 */
static int feed(const char *raw)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -99;
    net_write_all(sv[1], raw, strlen(raw));
    close(sv[1]);
    net_reader_t r;
    net_reader_init(&r, sv[0]);
    int st = http_read_request(&r, &req);
    close(sv[0]);
    return st;
}

static void test_ok(void)
{
    CHECK(feed("GET /docs/a%20b.html?x=1&y=%41 HTTP/1.1\r\n"
               "Host: localhost:8080\r\n"
               "user-agent:  curl/8  \r\n"
               "\r\n") == 0);
    CHECK(req.method == HTTP_GET);
    CHECK_STR(req.path, "/docs/a b.html");        /* %20 → 공백 */
    CHECK_STR(req.query, "x=1&y=%41");            /* 쿼리는 원문 유지 (CGI가 해석) */
    CHECK(req.minor == 1);
    CHECK(req.nheaders == 2);
    CHECK_STR(http_get_header(&req, "HOST"), "localhost:8080");   /* 대소문자 무시 */
    CHECK_STR(http_get_header(&req, "User-Agent"), "curl/8");     /* 앞뒤 공백 제거 */
    CHECK(http_get_header(&req, "Cookie") == NULL);
    CHECK(req.content_length == -1);
    CHECK(req.keep_alive == 1);                   /* 1.1 기본 = 유지 */

    CHECK(feed("POST /form HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n"
               "Connection: close\r\n\r\nhello=world") == 0);
    CHECK(req.method == HTTP_POST);
    CHECK(req.content_length == 11);
    CHECK(req.keep_alive == 0);

    CHECK(feed("GET / HTTP/1.0\r\n\r\n") == 0);   /* 1.0은 Host 없어도 됨 */
    CHECK(req.keep_alive == 0);                   /* 1.0 기본 = 끊음 */
    CHECK(feed("HEAD / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n") == 0);
    CHECK(req.method == HTTP_HEAD && req.keep_alive == 1);

    CHECK(feed("\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n") == 0);   /* 앞의 빈 줄 허용 */

    CHECK(feed("GET http://example.com:8001/api/x?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n") == 0);
    CHECK(req.is_absolute == 1);
    CHECK_STR(req.abs_host, "example.com");
    CHECK(req.abs_port == 8001);
    CHECK_STR(req.path, "/api/x");
    CHECK_STR(req.query, "q=1");
}

static void test_bad(void)
{
    /* 요청 줄 형식 */
    CHECK(feed("GET /\r\n\r\n") == 400);                          /* 부분 누락 */
    CHECK(feed("GET  / HTTP/1.1\r\nHost: a\r\n\r\n") == 400);     /* 공백 두 칸 */
    CHECK(feed("G@T / HTTP/1.1\r\nHost: a\r\n\r\n") == 400);      /* 비정상 메서드 */
    CHECK(feed("PUT / HTTP/1.1\r\nHost: a\r\n\r\n") == 501);      /* 미지원 메서드 */
    CHECK(feed("GET index.html HTTP/1.1\r\nHost: a\r\n\r\n") == 400);  /* '/'로 시작 안 함 */
    CHECK(feed("GET / HTTP/2.0\r\nHost: a\r\n\r\n") == 505);      /* 미지원 버전 */
    CHECK(feed("GET / HTTP/1.1x\r\nHost: a\r\n\r\n") == 400);     /* 잘못된 버전 형식 */
    CHECK(feed("GET / http/1.1\r\nHost: a\r\n\r\n") == 400);
    CHECK(feed("GET /a%zz HTTP/1.1\r\nHost: a\r\n\r\n") == 400);  /* 잘못된 % 인코딩 */
    CHECK(feed("GET /a%00.html HTTP/1.1\r\nHost: a\r\n\r\n") == 400);  /* 널 바이트 주입 */

    /* 헤더 */
    CHECK(feed("GET / HTTP/1.1\r\n\r\n") == 400);                 /* 1.1인데 Host 없음 */
    CHECK(feed("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n") == 400);
    CHECK(feed("GET / HTTP/1.1\r\nHost a\r\n\r\n") == 400);       /* 콜론 없음 */
    CHECK(feed("GET / HTTP/1.1\r\nHost : a\r\n\r\n") == 400);     /* 이름 뒤 공백 */
    CHECK(feed("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n") == 400);
    CHECK(feed("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5x\r\n\r\n") == 400);
    CHECK(feed("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n"
               "Transfer-Encoding: chunked\r\n\r\n") == 400);     /* 스머글링 방지 */
    CHECK(feed("GET / HTTP/1.1\r\nHost: a\r\nX: 1\r\n") == 400); /* 불완전한 헤더(빈 줄 전에 끊김) */
    CHECK(feed("") == NET_EOF);                                   /* 아무것도 안 보냄 */

    /* 헤더 개수 초과 */
    char big[8192] = "GET / HTTP/1.1\r\nHost: a\r\n";
    for (int i = 0; i < HTTP_MAX_HEADERS + 1; i++) strcat(big, "X-A: 1\r\n");
    strcat(big, "\r\n");
    CHECK(feed(big) == 431);
}

static void test_decode(void)
{
    char out[16];
    CHECK(http_url_decode("a%2Fb", 5, out, sizeof(out)) == 0);
    CHECK_STR(out, "a/b");
    CHECK(http_url_decode("%2", 2, out, sizeof(out)) == -1);       /* 끝이 잘림 */
    CHECK(http_url_decode("0123456789abcdefg", 17, out, sizeof(out)) == -1);  /* 버퍼 부족 */
}

void test_http(void)
{
    test_ok();
    test_bad();
    test_decode();
}

/* ---- 4단계: 본문 읽기 ---- */

/* feed_body — 요청 원문을 보내고 헤더+본문을 읽은 결과 코드를 돌려준다. close_after=0이면 보낸 쪽을 열어 둠 */
static int feed_body(const char *raw, size_t rawlen, size_t max, int *after_ok)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -99;
    net_write_all(sv[1], raw, rawlen);
    shutdown(sv[1], SHUT_WR);                 /* "더 보낼 것 없음" 표시 = 본문이 덜 오면 EOF */
    net_reader_t r;
    net_reader_init(&r, sv[0]);
    int st = http_read_request(&r, &req);
    if (st == 0) st = http_read_body(&r, &req, max);
    if (after_ok) {                           /* 본문 뒤에 붙은 다음 요청을 정확히 읽을 수 있는지 */
        char *b = req.body; size_t bl = req.body_len;   /* 다음 요청 해석 전에 본문 보관 */
        static http_request_t next;
        *after_ok = (http_read_request(&r, &next) == 0 && strcmp(next.path, "/next") == 0);
        req.body = b; req.body_len = bl;
    }
    close(sv[0]); close(sv[1]);
    return st;
}

#define FB(s, max, a) feed_body((s), sizeof(s) - 1, (max), (a))

static void test_body(void)
{
    int next_ok = 0;
    CHECK(FB("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhelloGET /next HTTP/1.1\r\nHost: a\r\n\r\n",
             100, &next_ok) == 0);
    CHECK(req.body_len == 5 && memcmp(req.body, "hello", 5) == 0);
    CHECK(next_ok);                            /* 본문 뒤 다음 요청 경계가 정확함 (keep-alive 전제) */
    http_free_body(&req);

    /* chunked: "Wiki" + "pedia" + 확장(;x=1) + 트레일러 */
    CHECK(FB("POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
             "4\r\nWiki\r\n5;x=1\r\npedia\r\n0\r\nX-T: 1\r\n\r\nGET /next HTTP/1.1\r\nHost: a\r\n\r\n", 100, &next_ok) == 0);
    CHECK(req.body_len == 9 && strcmp(req.body, "Wikipedia") == 0);
    CHECK(next_ok);
    http_free_body(&req);

    /* 바이너리(중간에 0 바이트)도 길이 기준이라 잘리지 않음 */
    CHECK(FB("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\na\0b", 100, NULL) == 0);
    CHECK(req.body_len == 3 && req.body[1] == '\0' && req.body[2] == 'b');
    http_free_body(&req);

    CHECK(FB("GET / HTTP/1.1\r\nHost: a\r\n\r\n", 100, NULL) == 0);
    CHECK(req.body == NULL && req.body_len == 0);              /* 본문 없음 */

    /* 예외 상황 */
    CHECK(FB("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 101\r\n\r\nx", 100, NULL) == 413);   /* 미리 거절 */
    CHECK(FB("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
             "40\r\n0123456789012345678901234567890123456789012345678901234567890123\r\n"
             "40\r\n0123456789012345678901234567890123456789012345678901234567890123\r\n0\r\n\r\n", 100, NULL) == 413);
    CHECK(FB("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nshort", 100, NULL) == 400);  /* 덜 오고 끊김 */
    CHECK(FB("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nab\r\n0\r\n\r\n", 100, NULL) == 400);
    CHECK(FB("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabc\r\n0\r\n\r\n", 100, NULL) == 400); /* 크기와 다름 */
    CHECK(FB("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab", 100, NULL) == 400);
    CHECK(FB("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n", 100, NULL) == 501);

    /* 본문이 덜 온 채로 멈춤 → 마감 시각이 지나면 408 */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    const char *slow = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc";
    net_write_all(sv[1], slow, strlen(slow));              /* 연결은 열어 둔 채 멈춤 */
    net_set_timeout(sv[0], 1);
    net_reader_t r;
    net_reader_init(&r, sv[0]);
    net_reader_set_deadline(&r, 2);
    CHECK(http_read_request(&r, &req) == 0);
    CHECK(http_read_body(&r, &req, 100) == 408);
    CHECK(req.body == NULL);                                /* 실패하면 메모리 반납됨 */
    close(sv[0]); close(sv[1]);
}

void test_http_body(void) { test_body(); }
