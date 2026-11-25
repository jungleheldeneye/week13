/*
 * conn.c — 연결 하나의 요청 처리 (conn.h 설명 참고)
 *
 * conn_serve_one 흐름:
 *   시간 제한 설정 → 헤더 읽기 → 본문 읽기 → (오류면 오류 응답 후 닫기)
 *   → keep-alive 판단 → 라우팅(Reverse Proxy / Forward Proxy / CGI / echo / 정적 파일) → 로그 → 본문 메모리 반납
 */
#include "conn.h"
#include "server.h"
#include "http.h"
#include "response.h"
#include "static_file.h"
#include "echo.h"
#include "proxy.h"
#include "cgi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

conn_t *conn_new(int fd, const struct sockaddr *sa, socklen_t salen)
{
    conn_t *c = malloc(sizeof(*c));
    if (!c) return NULL;
    c->fd = fd;
    net_set_nodelay(fd);        /* CGI/프록시처럼 여러 번 나눠 쓰는 응답에서 40ms 지연 방지 */
    net_peer_str(sa, salen, c->peer, sizeof(c->peer));
    net_reader_init(&c->r, fd);
    c->served = 0;
    c->last = time(NULL);
    return c;
}

void conn_free(conn_t *c)
{
    if (!c) return;
    close(c->fd);       /* 연결마다 반드시 닫기: 안 닫으면 fd 누수 */
    free(c);
}

int conn_serve_one(conn_t *c, const server_config_t *cfg, const char *who)
{
    static _Thread_local http_request_t req;   /* 약 40KB라 스택 대신 정적 영역 (스레드마다 따로) */
    int fd = c->fd;

    /* 첫 요청은 10초, 이후 "다음 요청을 기다리는" 시간은 5초 (빈 연결이 서버를 붙잡지 않게) */
    net_set_timeout(fd, c->served == 0 ? SERVER_TIMEOUT_S : SERVER_KEEPALIVE_S);
    /* 요청 하나(헤더 + 본문)를 다 받는 데 걸릴 수 있는 최대 시간: 느리게 보내는 공격 방어 */
    net_reader_set_deadline(&c->r, SERVER_REQUEST_DEADLINE_S);

    int st = http_read_request(&c->r, &req);
    if (st < 0) return 0;                         /* 끊김/대기 시간 초과: 응답 없이 종료 */
    if (st == 0) st = http_read_body(&c->r, &req, HTTP_MAX_BODY);
    if (st > 0) {                                 /* 잘못된 요청/본문 오류: 오류 응답 후 연결 종료
                                                   * (읽다 만 데이터가 남아 다음 요청 경계를 알 수 없으므로) */
        resp_error(fd, st, 0, req.method == HTTP_HEAD, NULL);
        printf("[%s] %s \"%s %s\" %d (요청 거절)\n", who, c->peer,
               req.method_str[0] ? req.method_str : "-", req.target[0] ? req.target : "-", st);
        http_free_body(&req);
        return 0;
    }
    net_reader_set_deadline(&c->r, 0);            /* 응답 중에는 수신 마감 해제 */

    /* 계속 유지할지: 클라이언트가 원하고, 상한 전이고, 종료 중이 아닐 때 */
    int keep = req.keep_alive && c->served + 1 < SERVER_KEEPALIVE_MAX && !server_stopping();

    /* 라우팅: 경로에 따라 처리기를 고른다 (6단계 Proxy, 7단계 CGI가 여기에 추가됨) */
    /*   우선순위: Reverse Proxy 규칙 → Forward Proxy(절대 URL, -f) → CGI(/cgi-bin/) → /echo → 정적 파일 */
    long long sent = 0;
    int code, route;
    char ip[64];
    snprintf(ip, sizeof(ip), "%s", c->peer);
    char *colon = strrchr(ip, ':');                /* "127.0.0.1:50312" → "127.0.0.1" */
    if (colon) *colon = '\0';

    if ((route = proxy_match(&req)) >= 0)
        code = proxy_reverse(fd, route, &req, ip, cfg->port, keep, &keep, &sent);
    else if (req.is_absolute && cfg->forward)
        code = proxy_forward(fd, &req, ip, cfg->port, keep, &keep, &sent);
    else if (cgi_is_request(&req))
        code = cgi_serve(fd, cfg, &req, c->peer, keep, &keep, &sent);
    else if (strcmp(req.path, ECHO_PATH) == 0)
        code = echo_serve(fd, &req, keep, &sent);
    else
        code = static_serve(fd, cfg->docroot, &req, keep, &sent);   /* 절대 URL이어도 -f가 없으면 우리 파일로 응답 (RFC 규칙) */

    printf("[%s] %s \"%s %s HTTP/1.%d\" %d %lld%s\n",
           who, c->peer, req.method_str, req.target, req.minor, code, sent,
           req.body_len ? " (본문 받음)" : "");
    http_free_body(&req);

    c->served++;
    c->last = time(NULL);
    return (code >= 0 && keep) ? 1 : 0;
}

void conn_run_blocking(conn_t *c, const server_config_t *cfg, const char *who)
{
    while (conn_serve_one(c, cfg, who))
        ;
    conn_free(c);
}
