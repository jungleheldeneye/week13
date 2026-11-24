#ifndef WEBSERVER_ROUTER_H
#define WEBSERVER_ROUTER_H

#include "http.h"
#include "proxy.h"

#include <sys/types.h>

typedef enum {
    ROUTE_STATIC,
    ROUTE_PROXY
} route_kind;

typedef struct {
    route_kind kind;
    char path[HTTP_MAX_URI_BYTES + 1];
} http_route;

typedef struct {
    int root_fd;
    const char *proxy_prefix;
    reverse_proxy *proxy;
    thread_pool *workers;
} router;

typedef struct {
    int fd;
    off_t length;
    const char *content_type;
} static_file;

int router_init(router *routes, const char *document_root);
void router_destroy(router *routes);
/* 0은 정상 분류, 양수는 HTTP 오류 상태다. 쿼리는 파일 경로에서 제외한다. */
int router_match(const http_request *request, http_route *route);
/* 응답까지 완료하면 0, 전송/파일 읽기 실패는 -1. 실패 후 응답을 덧붙이지 않는다. */
/* request->keep_alive는 실제 응답의 연결 정책으로 갱신된다. */
int router_handle(router *routes, int client_fd, http_request *request);

const char *static_file_mime_type(const char *path);
/* root_fd를 기준으로 파일을 열며 경로 구성요소마다 심볼릭 링크를 거절한다. */
int static_file_open(int root_fd, const char *path, static_file *file);
void static_file_close(static_file *file);
int static_file_serve(int client_fd, int root_fd, const char *path, bool *keep_alive);

#endif
