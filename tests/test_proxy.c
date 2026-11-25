/*
 * test_proxy.c — 라우팅 설정 파일 해석과 규칙 매칭 테스트
 *   (실제 중계 동작은 백엔드를 띄워야 해서 curl 시연/통합 테스트로 확인)
 */
#include "test.h"
#include "../src/proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static http_request_t rq;

/* write_conf — 임시 설정 파일을 만들고 proxy_init 결과를 돌려준다 */
static int load(const char *text)
{
    char path[] = "/tmp/cweb_routes_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -99;
    if (write(fd, text, strlen(text)) < 0) { close(fd); return -99; }
    close(fd);
    char err[256];
    int rc = proxy_init(path, err, sizeof(err));
    unlink(path);
    return rc;
}

/* req — 경로와 Host만 채운 가짜 요청 */
static const http_request_t *req(const char *path, const char *host)
{
    memset(&rq, 0, offsetof(http_request_t, hbuf));
    rq.hused = 0;
    strcpy(rq.path, path);
    if (host) {
        char line[300];
        snprintf(line, sizeof(line), "Host: %s", host);
        http_parse_header_line(line, &rq);
    }
    return &rq;
}

void test_proxy(void)
{
    CHECK(load("# 주석\n\nroute /api/ 127.0.0.1:8001 127.0.0.1:8002\n"
               "route /api/v2/ [::1]:9000 strip\n"
               "host api.local 127.0.0.1:8001\n") == 0);
    CHECK(proxy_route_count() == 3);
    CHECK(proxy_match(req("/api/users", "x")) == 0);
    CHECK(proxy_match(req("/api", "x")) == 0);            /* 끝 '/' 없는 경로도 매칭 */
    CHECK(proxy_match(req("/api/v2/a", "x")) == 1);       /* 더 긴 접두어 우선 */
    CHECK(proxy_match(req("/apix", "x")) == -1);
    CHECK(proxy_match(req("/index.html", "x")) == -1);
    CHECK(proxy_match(req("/index.html", "API.local:8080")) == 2);   /* Host 규칙, 대소문자·포트 무시 */

    CHECK(load("route api 127.0.0.1:8001\n") == -1);         /* '/'로 시작 안 함 */
    CHECK(load("route /a/ 127.0.0.1\n") == -1);              /* 포트 없음 */
    CHECK(load("route /a/ 127.0.0.1:70000\n") == -1);        /* 포트 범위 */
    CHECK(load("route /a/\n") == -1);                        /* 백엔드 없음 */
    CHECK(load("proxy /a/ 127.0.0.1:8001\n") == -1);         /* 모르는 지시어 */
    CHECK(proxy_init("", NULL, 0) == 0 && proxy_route_count() == 0);   /* 설정 없음 */
}
