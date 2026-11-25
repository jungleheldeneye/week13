/*
 * test_cache.c — Forward Proxy 캐시(cache.c)와 CGI 출력 해석(cgi_parse_headers) 테스트
 */
#include "test.h"
#include "../src/cache.h"
#include "../src/cgi.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static http_request_t rq;

/* mkreq — 메서드와 헤더 줄들("A: 1\nB: 2")로 가짜 요청 만들기 */
static const http_request_t *mkreq(http_method_t m, const char *headers)
{
    memset(&rq, 0, offsetof(http_request_t, hbuf));
    rq.hused = 0;
    rq.method = m;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", headers);
    for (char *save, *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
        http_parse_header_line(l, &rq);
    return &rq;
}

static void test_cache_rules(void)
{
    /* 유효 시간 결정 */
    CHECK(cache_ttl(NULL, 0, 0) == CACHE_DEFAULT_TTL);
    CHECK(cache_ttl("max-age=120", 0, 0) == 120);
    CHECK(cache_ttl("public, max-age=10, s-maxage=50", 0, 0) == 50);   /* 프록시용 값 우선 */
    CHECK(cache_ttl("max-age=0", 0, 0) == -1);
    CHECK(cache_ttl("no-store", 0, 0) == -1);
    CHECK(cache_ttl("private, max-age=60", 0, 0) == -1);
    CHECK(cache_ttl("No-Cache", 0, 0) == -1);                          /* 대소문자 무시 */
    CHECK(cache_ttl("max-age=99999999", 0, 0) == 86400);               /* 하루로 제한 */
    CHECK(cache_ttl(NULL, 1, 0) == -1);                                /* Set-Cookie */
    CHECK(cache_ttl(NULL, 0, 1) == -1);                                /* Vary */

    /* 캐시에서 찾아도 되는 요청 */
    CHECK(cache_request_ok(mkreq(HTTP_GET, "Host: a")) == 1);
    CHECK(cache_request_ok(mkreq(HTTP_HEAD, "Host: a")) == 1);
    CHECK(cache_request_ok(mkreq(HTTP_POST, "Host: a")) == 0);
    CHECK(cache_request_ok(mkreq(HTTP_GET, "Cache-Control: no-cache")) == 0);
    CHECK(cache_request_ok(mkreq(HTTP_GET, "Pragma: no-cache")) == 0);
    CHECK(cache_request_ok(mkreq(HTTP_GET, "Authorization: Basic eA==")) == 0);
}

static void test_cache_store(void)
{
    cache_clear();
    cache_hit_t h;
    const char *head = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n";
    CHECK(cache_get("http://a:80/x", &h) == 0);                       /* 처음엔 없음 */
    cache_put("http://a:80/x", head, strlen(head), "hello", 5, 60);
    CHECK(cache_get("http://a:80/x", &h) == 1);
    CHECK(h.body_len == 5 && memcmp(h.body, "hello", 5) == 0);
    CHECK(h.head_len == strlen(head) && h.age == 0);
    cache_hit_free(&h);
    CHECK(cache_get("http://a:80/y", &h) == 0);                       /* 다른 URL */

    cache_put("http://a:80/x", head, strlen(head), "world", 5, 60);    /* 같은 URL → 교체 */
    CHECK(cache_get("http://a:80/x", &h) == 1 && memcmp(h.body, "world", 5) == 0);
    cache_hit_free(&h);

    static char big[CACHE_MAX_OBJECT + 1];
    cache_put("http://a:80/big", head, strlen(head), big, sizeof(big), 60);   /* 1MB 초과 → 저장 안 함 */
    CHECK(cache_get("http://a:80/big", &h) == 0);
    cache_put("http://a:80/z", head, strlen(head), "z", 1, 0);         /* ttl 0 → 저장 안 함 */
    CHECK(cache_get("http://a:80/z", &h) == 0);

    /* LRU: 개수 한도를 넘기면 가장 오래 안 쓴 것부터 사라짐 */
    cache_clear();
    char key[64];
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        snprintf(key, sizeof(key), "http://a:80/%d", i);
        cache_put(key, head, strlen(head), "x", 1, 60);
    }
    CHECK(cache_get("http://a:80/0", &h) == 1);                        /* 0번을 방금 사용 → 최근 쪽으로 */
    cache_hit_free(&h);
    cache_put("http://a:80/new", head, strlen(head), "x", 1, 60);      /* 한도 초과 → 1번이 밀려남 */
    CHECK(cache_get("http://a:80/1", &h) == 0);
    CHECK(cache_get("http://a:80/0", &h) == 1);
    cache_hit_free(&h);
    long hits, misses;
    int n;
    size_t bytes;
    cache_stats(&hits, &misses, &n, &bytes);
    CHECK(n == CACHE_MAX_ENTRIES && hits == 2 && misses == 1);
    cache_clear();
    cache_stats(&hits, &misses, &n, &bytes);
    CHECK(n == 0 && bytes == 0);
}

static void test_cgi_headers(void)
{
    char out[1024], reason[128];
    char h1[] = "Content-Type: text/html\nX-A: 1";
    CHECK(cgi_parse_headers(h1, out, sizeof(out), reason, sizeof(reason)) == 200);
    CHECK_STR(out, "Content-Type: text/html\r\nX-A: 1\r\n");
    CHECK_STR(reason, "OK");

    char h2[] = "Status: 404 Not Here\r\nContent-Type: text/plain\r";
    CHECK(cgi_parse_headers(h2, out, sizeof(out), reason, sizeof(reason)) == 404);
    CHECK_STR(reason, "Not Here");

    char h3[] = "Location: /next";                                 /* Location만 → 302 */
    CHECK(cgi_parse_headers(h3, out, sizeof(out), reason, sizeof(reason)) == 302);

    char h4[] = "Content-Type: a\nContent-Length: 9\nConnection: close\nTransfer-Encoding: x";
    CHECK(cgi_parse_headers(h4, out, sizeof(out), reason, sizeof(reason)) == 200);
    CHECK_STR(out, "Content-Type: a\r\n");                         /* 길이·연결 헤더는 서버가 정함 */

    char h5[] = "hello world";                                     /* 헤더가 아님 */
    CHECK(cgi_parse_headers(h5, out, sizeof(out), reason, sizeof(reason)) == -1);
    char h6[] = "X-Only: 1";                                       /* Content-Type/Location/Status 없음 */
    CHECK(cgi_parse_headers(h6, out, sizeof(out), reason, sizeof(reason)) == -1);
    char h7[] = "Status: 999\nContent-Type: a";                    /* 상태 코드 범위 밖 */
    CHECK(cgi_parse_headers(h7, out, sizeof(out), reason, sizeof(reason)) == -1);
    char h8[] = "Bad Name: 1\nContent-Type: a";                    /* 이름에 공백 */
    CHECK(cgi_parse_headers(h8, out, sizeof(out), reason, sizeof(reason)) == -1);
}

void test_cache(void)
{
    test_cache_rules();
    test_cache_store();
    test_cgi_headers();
}
