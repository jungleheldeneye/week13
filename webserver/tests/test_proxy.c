#include "proxy.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void test_responses(void)
{
    static const struct { const char *wire; int expected; } cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", 0},
        {"HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\n", 0},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", 0},
        {"HTTP/1.1 200 OK\r\ntransfer-encoding: ChUnKeD\r\n\r\n", 0},
        {"HTTP/1.1 204 No Content\r\n\r\n", 0},
        {"HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n", 0},
        {"HTTP/1.1 103 Early Hints\r\nLink: </a.css>\r\n\r\n", 0},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive, X-Hop\r\n\r\n", 0},
        {"HTTP/1.1 200 \r\n\r\n", 0},
        {"HTTP/1.2 200 OK\r\n\r\n", 502},
        {"HTTP/1.1 600 Bad\r\n\r\n", 502},
        {"HTTP/1.1 099 Bad\r\n\r\n", 502},
        {"HTTP/1.1 2a0 Bad\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999999999\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nContent-Length: 1, 1\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nContent-Length : 1\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nInvalid Header\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nX: a\001b\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\n X: folded\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nConnection: close,\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\r\nConnection: a b\r\n\r\n", 502},
        {"HTTP/1.1 204 No Content\r\nContent-Length: 1\r\n\r\n", 502},
        {"HTTP/1.1 100 Continue\r\nTransfer-Encoding: chunked\r\n\r\n", 502},
        {"HTTP/1.1 200 OK\nX: a\n\n", 502},
        {"HTTP/1.1 200 OK\r\nX: a\r\n", 502},
        {"HTTP/1.1 200 OK\r\n\r\nX: b\r\n\r\n", 502}
    };
    proxy_response response;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(proxy_parse_response((const unsigned char *)cases[i].wire, strlen(cases[i].wire), &response) == cases[i].expected);
    }
    CHECK(proxy_parse_response((const unsigned char *)cases[0].wire, strlen(cases[0].wire), &response) == 0);
    CHECK(response.status == 200 && response.has_length && response.length == 5 && !response.chunked);
    CHECK(response.header_count == 1 && strcmp(response.headers[0].name, "Content-Length") == 0);
    CHECK(proxy_parse_response(NULL, 4, &response) == 502);
    unsigned char data[HTTP_MAX_HEADER_BYTES + 4];
    const char *prefix = "HTTP/1.1 200 OK\r\nX-Pad: ";
    memset(data, 'a', sizeof(data));
    memcpy(data, prefix, strlen(prefix));
    memcpy(data + HTTP_MAX_HEADER_BYTES - 4, "\r\n\r\n", 4);
    CHECK(proxy_parse_response(data, HTTP_MAX_HEADER_BYTES, &response) == 0);
    CHECK(proxy_parse_response(data, sizeof(data), &response) == 502);
    data[30] = '\0';
    CHECK(proxy_parse_response(data, HTTP_MAX_HEADER_BYTES, &response) == 502);
    size_t used = (size_t)sprintf((char *)data, "HTTP/1.1 200 OK\r\n");
    for (unsigned i = 0; i <= HTTP_MAX_HEADERS; ++i) used += (size_t)sprintf((char *)data + used, "X: a\r\n");
    memcpy(data + used, "\r\n", 2);
    CHECK(proxy_parse_response(data, used + 2, &response) == 502);
}

typedef struct { reverse_proxy *proxy; atomic_uint *counts; } selector;

static void *select_backends(void *argument)
{
    selector *job = argument;

    for (unsigned i = 0; i < 1000; ++i) {
        backend_config backend;

        CHECK(proxy_next_backend(job->proxy, &backend) == 0);
        CHECK(backend.port == 8001 || backend.port == 8002);
        atomic_fetch_add(&job->counts[backend.port - 8001], 1);
    }
    return NULL;
}

static void test_round_robin(void)
{
    server_config config = {.proxy_timeout_ms = 5000};
    reverse_proxy proxy;
    backend_config backend;
    pthread_t threads[4];
    atomic_uint counts[2] = {0};
    selector job = {&proxy, counts};

    CHECK(proxy_init(&proxy, &config) == 0);
    CHECK(proxy_next_backend(&proxy, &backend) == -1);
    proxy_destroy(&proxy);
    config.backend_count = 2;
    config.backends[0] = (backend_config){"127.0.0.1", 8001};
    config.backends[1] = (backend_config){"127.0.0.1", 8002};
    CHECK(proxy_init(&proxy, &config) == 0);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(proxy_next_backend(&proxy, &backend) == 0 && backend.port == 8001 + i % 2);
    }
    for (unsigned i = 0; i < 4; ++i) CHECK(pthread_create(&threads[i], NULL, select_backends, &job) == 0);
    for (unsigned i = 0; i < 4; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(atomic_load(&counts[0]) == 2000 && atomic_load(&counts[1]) == 2000);
    proxy_destroy(&proxy);
}

int main(void)
{
    test_responses();
    test_round_robin();
    puts("PASS: upstream response framing/status/headers/bounds, concurrent Round-Robin");
    return EXIT_SUCCESS;
}
