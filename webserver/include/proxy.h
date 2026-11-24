#ifndef WEBSERVER_PROXY_H
#define WEBSERVER_PROXY_H

#include "config.h"
#include "http.h"
#include "thread_pool.h"

#include <pthread.h>

typedef struct {
    backend_config backends[CONFIG_MAX_BACKENDS];
    size_t count;
    size_t next;
    unsigned timeout_ms;
    pthread_mutex_t mutex;
} reverse_proxy;

typedef struct {
    int status;
    bool chunked;
    bool has_length;
    size_t length;
    size_t header_count;
    http_header headers[HTTP_MAX_HEADERS];
    char *status_line;
    char storage[HTTP_MAX_HEADER_BYTES + 1];
} proxy_response;

int proxy_init(reverse_proxy *proxy, const server_config *config);
void proxy_destroy(reverse_proxy *proxy);
/* 0: 선택 완료, -1: 백엔드 없음. 인덱스 갱신은 mutex로 보호한다. */
int proxy_next_backend(reverse_proxy *proxy, backend_config *backend);
/* 0: 정상, 502: 잘못된 백엔드 응답. length는 헤더 끝까지의 길이. */
int proxy_parse_response(const unsigned char *data, size_t length, proxy_response *response);
/* headers 검증 후 본문을 중계한다. 응답 시작 후 실패하면 -1이며 추가 응답을 보내지 않는다. */
int proxy_serve(reverse_proxy *proxy, int client_fd, http_request *request,
                thread_pool *workers);

#endif
