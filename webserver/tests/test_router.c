#include "router.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static unsigned route_cases;

static void test_routes(void)
{
    static const struct {
        const char *target;
        int status;
        route_kind kind;
        const char *path;
    } cases[] = {
        {"/", 0, ROUTE_STATIC, "/index.html"},
        {"/?q=1", 0, ROUTE_STATIC, "/index.html"},
        {"/photo.jpg?v=1", 0, ROUTE_STATIC, "/photo.jpg"},
        {"/docs/", 0, ROUTE_STATIC, "/docs/index.html"},
        {"//docs//./file.txt", 0, ROUTE_STATIC, "/docs/file.txt"},
        {"/hello%20world.txt", 0, ROUTE_STATIC, "/hello world.txt"},
        {"/%ED%95%9C%EA%B8%80.txt", 0, ROUTE_STATIC, "/한글.txt"},
        {"/api", 0, ROUTE_PROXY, "/api"},
        {"/api/users?q=1", 0, ROUTE_PROXY, "/api/users"},
        {"/%61pi/users", 0, ROUTE_PROXY, "/api/users"},
        {"/api/", 0, ROUTE_PROXY, "/api"},
        {"/apix.txt", 0, ROUTE_STATIC, "/apix.txt"},
        {"/../secret.txt", 403, ROUTE_STATIC, ""},
        {"/a/../secret.txt", 403, ROUTE_STATIC, ""},
        {"/%2e%2e/secret.txt", 403, ROUTE_STATIC, ""},
        {"/%2E%2e%2fsecret.txt", 403, ROUTE_STATIC, ""},
        {"/%5csecret.txt", 403, ROUTE_STATIC, ""},
        {"/a%00.txt", 400, ROUTE_STATIC, ""},
        {"/a%0d.txt", 400, ROUTE_STATIC, ""},
        {"/a%", 400, ROUTE_STATIC, ""},
        {"/a%xy", 400, ROUTE_STATIC, ""},
        {"relative", 400, ROUTE_STATIC, ""},
        {"/%252e%252e/file", 0, ROUTE_STATIC, "/%2e%2e/file"}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        http_request request;
        http_route route;

        http_request_init(&request);
        strcpy(request.target, cases[i].target);
        CHECK(router_match(&request, &route) == cases[i].status);
        if (cases[i].status == 0) {
            CHECK(route.kind == cases[i].kind);
            CHECK(strcmp(route.path, cases[i].path) == 0);
        }
        ++route_cases;
    }
    {
        http_request request;
        http_route route;

        http_request_init(&request);
        memset(request.target, 'a', HTTP_MAX_URI_BYTES);
        request.target[0] = '/';
        request.target[HTTP_MAX_URI_BYTES - 1] = '/';
        request.target[HTTP_MAX_URI_BYTES] = '\0';
        CHECK(router_match(&request, &route) == 414);
    }
}

static void test_mime(void)
{
    static const struct { const char *path; const char *type; } cases[] = {
        {"/index.HTML", "text/html; charset=utf-8"},
        {"/style.css", "text/css; charset=utf-8"},
        {"/app.js", "text/javascript; charset=utf-8"},
        {"/data.json", "application/json; charset=utf-8"},
        {"/text.txt", "text/plain; charset=utf-8"},
        {"/photo.png", "image/png"},
        {"/photo.jpg", "image/jpeg"},
        {"/photo.JPEG", "image/jpeg"},
        {"/mark.svg", "image/svg+xml"},
        {"/image.webp", "image/webp"},
        {"/icon.ico", "image/x-icon"},
        {"/report.pdf", "application/pdf"},
        {"/archive.bin", "application/octet-stream"},
        {"/dir.css/file", "application/octet-stream"}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(strcmp(static_file_mime_type(cases[i].path), cases[i].type) == 0);
    }
}

typedef struct {
    int fd;
    unsigned char *data;
    size_t capacity;
    size_t used;
} response_reader;

static void *read_response(void *argument)
{
    response_reader *response = argument;

    while (response->used < response->capacity) {
        ssize_t count = recv(response->fd, response->data + response->used,
                             response->capacity - response->used, 0);

        if (count == -1 && errno == EINTR) continue;
        CHECK(count >= 0);
        if (count == 0) return NULL;
        response->used += (size_t)count;
    }
    CHECK(false);
    return NULL;
}

static size_t run_route(router *routes, const char *method, const char *target,
                        unsigned char *buffer, size_t capacity)
{
    int sockets[2];
    pthread_t thread;
    http_request request;
    response_reader response;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(http_set_socket_timeout(sockets[0], 3000) == 0);
    CHECK(http_set_socket_timeout(sockets[1], 3000) == 0);
    response = (response_reader){sockets[1], buffer, capacity - 1, 0};
    CHECK(pthread_create(&thread, NULL, read_response, &response) == 0);
    http_request_init(&request);
    strcpy(request.method, method);
    strcpy(request.target, target);
    CHECK(router_handle(routes, sockets[0], &request) == 0);
    CHECK(shutdown(sockets[0], SHUT_WR) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    buffer[response.used] = '\0';
    close(sockets[0]);
    close(sockets[1]);
    return response.used;
}

static void write_file(const char *path, const unsigned char *data, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t used = 0;

    CHECK(fd >= 0);
    while (used < length) {
        ssize_t count = write(fd, data + used, length - used);

        if (count == -1 && errno == EINTR) continue;
        CHECK(count > 0);
        used += (size_t)count;
    }
    CHECK(close(fd) == 0);
}

static void test_files_and_responses(void)
{
    char directory[] = "/tmp/week13-static-XXXXXX";
    char path[256];
    router routes;
    router broken = {.root_fd = -1};
    static_file file;
    size_t body_length = 256 * 1024 + 7;
    unsigned char *payload = malloc(body_length);
    unsigned char *response = malloc(body_length + 1024);
    size_t used;
    char *body;

    CHECK(payload != NULL && response != NULL);
    for (size_t i = 0; i < body_length; ++i) payload[i] = (unsigned char)(i % 251);
    CHECK(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/index.html", directory);
    write_file(path, (const unsigned char *)"<h1>ok</h1>", 11);
    snprintf(path, sizeof(path), "%s/binary.bin", directory);
    write_file(path, payload, body_length);
    snprintf(path, sizeof(path), "%s/empty.txt", directory);
    write_file(path, payload, 0);
    snprintf(path, sizeof(path), "%s/nested", directory);
    CHECK(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/nested/file.txt", directory);
    write_file(path, (const unsigned char *)"nested", 6);
    snprintf(path, sizeof(path), "%s/link", directory);
    CHECK(symlink("index.html", path) == 0);
    snprintf(path, sizeof(path), "%s/link-dir", directory);
    CHECK(symlink("nested", path) == 0);
    snprintf(path, sizeof(path), "%s/pipe", directory);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(router_init(&routes, directory) == 0);

    CHECK(static_file_open(routes.root_fd, "/nested/file.txt", &file) == 0);
    CHECK(file.length == 6);
    static_file_close(&file);
    CHECK(static_file_open(routes.root_fd, "/missing", &file) == 404);
    CHECK(file.fd == -1);
    CHECK(static_file_open(routes.root_fd, "/../secret", &file) == 403);
    CHECK(static_file_open(routes.root_fd, "/link", &file) == 403);
    CHECK(static_file_open(routes.root_fd, "/link-dir/file.txt", &file) == 403);
    CHECK(static_file_open(routes.root_fd, "/pipe", &file) == 403);
    CHECK(static_file_open(routes.root_fd, "/nested", &file) == 403);
    CHECK(static_file_open(routes.root_fd, "/index.html/other", &file) == 404);
    CHECK(static_file_open(-1, "/index.html", &file) == 500);

    used = run_route(&routes, "GET", "/binary.bin", response, body_length + 1024);
    CHECK(strncmp((char *)response, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr((char *)response, "Content-Type: application/octet-stream\r\n") != NULL);
    body = strstr((char *)response, "\r\n\r\n");
    CHECK(body != NULL);
    body += 4;
    CHECK(used - (size_t)(body - (char *)response) == body_length);
    CHECK(memcmp(body, payload, body_length) == 0);

    run_route(&routes, "GET", "/", response, body_length + 1024);
    CHECK(strstr((char *)response, "\r\n\r\n<h1>ok</h1>") != NULL);
    run_route(&routes, "GET", "/empty.txt", response, body_length + 1024);
    CHECK(strstr((char *)response, "Content-Length: 0\r\n") != NULL);
    CHECK(strcmp(strstr((char *)response, "\r\n\r\n"), "\r\n\r\n") == 0);
    run_route(&routes, "GET", "/missing", response, body_length + 1024);
    CHECK(strncmp((char *)response, "HTTP/1.1 404 ", 13) == 0);
    run_route(&routes, "GET", "/../secret", response, body_length + 1024);
    CHECK(strncmp((char *)response, "HTTP/1.1 403 ", 13) == 0);
    run_route(&routes, "POST", "/index.html", response, body_length + 1024);
    CHECK(strncmp((char *)response, "HTTP/1.1 405 ", 13) == 0);
    CHECK(strstr((char *)response, "Allow: GET\r\n") != NULL);
    run_route(&routes, "POST", "/api/query", response, body_length + 1024);
    CHECK(strncmp((char *)response, "HTTP/1.1 503 ", 13) == 0);
    run_route(&broken, "GET", "/index.html", response, body_length + 1024);
    CHECK(strncmp((char *)response, "HTTP/1.1 500 ", 13) == 0);

    router_destroy(&routes);
    CHECK(routes.root_fd == -1);
    const char *files[] = {"index.html", "binary.bin", "empty.txt", "nested/file.txt", "link", "link-dir", "pipe"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        CHECK(unlink(path) == 0);
    }
    snprintf(path, sizeof(path), "%s/nested", directory);
    CHECK(rmdir(path) == 0);
    CHECK(rmdir(directory) == 0);
    free(payload);
    free(response);
}

int main(void)
{
    CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    test_routes();
    test_mime();
    test_files_and_responses();
    printf("PASS: %u route cases, MIME, safe file opening, multi-block binary transfer, empty file and 403/404/405/500/503\n", route_cases);
    return EXIT_SUCCESS;
}
