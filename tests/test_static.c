/*
 * test_static.c — 응답·정적 파일 테스트
 *   경로 해석(정상/폴더/301/404/경로 순회 403), MIME 표, 날짜 형식,
 *   그리고 실제로 응답을 소켓에 보내 상태 줄·헤더·본문을 확인한다 (GET/HEAD/304/405).
 */
#include "test.h"
#include "../src/static_file.h"
#include "../src/response.h"

#include <limits.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static char root[PATH_MAX];
static http_request_t rq;

/* serve — 요청 원문을 해석해 static_serve로 응답하고, 응답 전체를 out에 받아 온다 */
static int serve(const char *raw, char *out, size_t outlen)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    net_write_all(sv[1], raw, strlen(raw));
    net_reader_t r;
    net_reader_init(&r, sv[0]);
    if (http_read_request(&r, &rq) != 0) { close(sv[0]); close(sv[1]); return -99; }
    long long sent;
    int code = static_serve(sv[0], root, &rq, 0, &sent);
    close(sv[0]);
    size_t got = 0;
    ssize_t k;
    while (got + 1 < outlen && (k = read(sv[1], out + got, outlen - 1 - got)) > 0) got += (size_t)k;
    out[got] = '\0';
    close(sv[1]);
    return code;
}

static void test_resolve(void)
{
    char out[PATH_MAX];
    struct stat st;
    CHECK(static_resolve(root, "/index.html", out, sizeof(out), &st) == 0);
    CHECK(strstr(out, "/www/index.html") != NULL);
    CHECK(static_resolve(root, "/", out, sizeof(out), &st) == 0);          /* 루트 폴더 → index.html */
    CHECK(static_resolve(root, "/docs/", out, sizeof(out), &st) == 0);
    CHECK(strstr(out, "/docs/index.html") != NULL);
    CHECK(static_resolve(root, "/docs", out, sizeof(out), &st) == 301);    /* '/' 없는 폴더 */
    CHECK(static_resolve(root, "/nope.html", out, sizeof(out), &st) == 404);
    CHECK(static_resolve(root, "/../Makefile", out, sizeof(out), &st) == 403);  /* 루트 밖 */
    CHECK(static_resolve(root, "/css/../../src/main.c", out, sizeof(out), &st) == 403);
    CHECK(static_resolve(root, "/index.html/x", out, sizeof(out), &st) == 404); /* 파일을 폴더처럼 */
    CHECK(static_resolve(root, "/img/", out, sizeof(out), &st) == 403);    /* index.html 없는 폴더 */
}

static void test_helpers(void)
{
    CHECK_STR(mime_type("/a/b.HTML"), "text/html; charset=utf-8");   /* 대소문자 무시 */
    CHECK_STR(mime_type("x.png"), "image/png");
    CHECK_STR(mime_type("/dir.v2/README"), "application/octet-stream");
    char d[64];
    http_date(0, d, sizeof(d));
    CHECK_STR(d, "Thu, 01 Jan 1970 00:00:00 GMT");

    /* writev: 세 조각이 순서대로 하나로 이어져 도착하는지 */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct iovec iov[3] = {{"ab", 2}, {"", 0}, {"cde", 3}};
    CHECK(net_writev_all(sv[0], iov, 3) == 0);
    char buf[8] = {0};
    CHECK(read(sv[1], buf, sizeof(buf)) == 5);
    CHECK_STR(buf, "abcde");
    close(sv[0]); close(sv[1]);
}

static void test_serve(void)
{
    static char out[70000];
    CHECK(serve("GET /css/style.css HTTP/1.1\r\nHost: a\r\n\r\n", out, sizeof(out)) == 200);
    CHECK(strncmp(out, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(out, "Content-Type: text/css; charset=utf-8\r\n") != NULL);
    CHECK(strstr(out, "Last-Modified: ") != NULL);
    CHECK(strstr(out, "\r\n\r\nbody {") != NULL);               /* 빈 줄 뒤에 파일 내용 */

    CHECK(serve("HEAD /index.html HTTP/1.1\r\nHost: a\r\n\r\n", out, sizeof(out)) == 200);
    char *end = strstr(out, "\r\n\r\n");
    CHECK(end && end[4] == '\0');                                /* HEAD: 본문 없음 */
    CHECK(strstr(out, "Content-Length: 0") == NULL);             /* 길이는 실제 파일 크기 */

    /* 304: 방금 받은 Last-Modified를 그대로 보내면 본문 없이 304 */
    char *lm = strstr(out, "Last-Modified: ") + 15;
    char ims[64];
    snprintf(ims, sizeof(ims), "%.*s", (int)strcspn(lm, "\r"), lm);
    char raw[256];
    snprintf(raw, sizeof(raw), "GET /index.html HTTP/1.1\r\nHost: a\r\nIf-Modified-Since: %s\r\n\r\n", ims);
    CHECK(serve(raw, out, sizeof(out)) == 304);
    CHECK(strncmp(out, "HTTP/1.1 304", 12) == 0);

    CHECK(serve("GET /nope HTTP/1.1\r\nHost: a\r\n\r\n", out, sizeof(out)) == 404);
    CHECK(strstr(out, "<h1>404 Not Found</h1>") != NULL);
    CHECK(serve("POST /index.html HTTP/1.1\r\nHost: a\r\n\r\n", out, sizeof(out)) == 405);
    CHECK(strstr(out, "Allow: GET, HEAD\r\n") != NULL);
    CHECK(serve("GET /docs?x=1 HTTP/1.1\r\nHost: a\r\n\r\n", out, sizeof(out)) == 301);
    CHECK(strstr(out, "Location: /docs/?x=1\r\n") != NULL);
    CHECK(serve("GET /%2e%2e/Makefile HTTP/1.1\r\nHost: a\r\n\r\n", out, sizeof(out)) == 403);  /* 인코딩된 ../ */
}

void test_static(void)
{
    CHECK(realpath("www", root) != NULL);
    test_resolve();
    test_helpers();
    test_serve();
}
