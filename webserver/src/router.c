#include "router.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

int router_init(router *routes, const char *document_root)
{
    routes->proxy_prefix = "/api";
    routes->proxy = NULL;
    routes->workers = NULL;
    routes->root_fd = open(document_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return routes->root_fd == -1 ? -1 : 0;
}

void router_destroy(router *routes)
{
    int saved_errno = errno;

    if (routes->root_fd >= 0) {
        close(routes->root_fd);
        routes->root_fd = -1;
    }
    errno = saved_errno;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_path(const char *target, char *decoded)
{
    size_t source_length = strcspn(target, "?");
    size_t used = 0;

    if (*target != '/') return 400;
    if (source_length > HTTP_MAX_URI_BYTES) return 414;
    for (size_t i = 0; i < source_length; ++i) {
        unsigned char c = (unsigned char)target[i];

        if (c == '%') {
            int high, low;

            if (i + 2 >= source_length) return 400;
            high = hex_digit((unsigned char)target[i + 1]);
            low = hex_digit((unsigned char)target[i + 2]);
            if (high < 0 || low < 0) return 400;
            c = (unsigned char)(high * 16 + low);
            i += 2;
        }
        if (c < 0x20 || c == 0x7f) return 400;
        if (c == '\\') return 403;
        decoded[used++] = (char)c;
    }
    decoded[used] = '\0';
    return 0;
}

static int match_prefix(const http_request *request, http_route *route, const char *prefix)
{
    char decoded[HTTP_MAX_URI_BYTES + 1];
    size_t used = 0;
    const char *cursor;
    bool directory;
    int status = decode_path(request->target, decoded);

    if (status != 0) return status;
    directory = decoded[strlen(decoded) - 1] == '/';
    cursor = decoded;
    while (*cursor != '\0') {
        const char *start;
        size_t length;

        while (*cursor == '/') ++cursor;
        if (*cursor == '\0') break;
        start = cursor;
        while (*cursor != '\0' && *cursor != '/') ++cursor;
        length = (size_t)(cursor - start);
        if (length == 1 && start[0] == '.') continue;
        if (length == 2 && start[0] == '.' && start[1] == '.') return 403;
        if (used + 1 + length > HTTP_MAX_URI_BYTES) return 414;
        route->path[used++] = '/';
        memcpy(route->path + used, start, length);
        used += length;
    }
    if (used == 0) route->path[used++] = '/';
    route->path[used] = '\0';

    if (strncmp(route->path, prefix, strlen(prefix)) == 0 &&
        (route->path[strlen(prefix)] == '\0' || route->path[strlen(prefix)] == '/')) {
        route->kind = ROUTE_PROXY;
        return 0;
    }
    route->kind = ROUTE_STATIC;
    if (directory) {
        const char *index = used == 1 ? "index.html" : "/index.html";
        size_t length = strlen(index);

        if (used + length > HTTP_MAX_URI_BYTES) return 414;
        memcpy(route->path + used, index, length + 1);
    }
    return 0;
}

int router_match(const http_request *request, http_route *route)
{
    return match_prefix(request, route, "/api");
}

int router_handle(router *routes, int client_fd, http_request *request)
{
    http_route route;
    int status = match_prefix(request, &route, routes->proxy_prefix != NULL ? routes->proxy_prefix : "/api");

    if (status != 0) {
        request->keep_alive = false;
        return http_send_error(client_fd, status);
    }
    if (route.kind == ROUTE_PROXY) {
        if (routes->proxy == NULL) {
            request->keep_alive = false;
            return http_send_error(client_fd, 503);
        }
        return proxy_serve(routes->proxy, client_fd, request, routes->workers);
    }
    if (strcmp(request->method, "GET") != 0) {
        request->keep_alive = false;
        return http_send_error_with_allow(client_fd, 405, "GET");
    }
    return static_file_serve(client_fd, routes->root_fd, route.path, &request->keep_alive);
}
