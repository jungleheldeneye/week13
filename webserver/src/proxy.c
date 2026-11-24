#include "proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { PROXY_CHUNK_BYTES = 16 * 1024, PROXY_WIRE_BYTES = HTTP_MAX_HEADER_BYTES + 4096 };

int proxy_init(reverse_proxy *proxy, const server_config *config)
{
    int error;

    if (config->backend_count > CONFIG_MAX_BACKENDS || config->proxy_timeout_ms == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(proxy, 0, sizeof(*proxy));
    proxy->count = config->backend_count;
    proxy->timeout_ms = config->proxy_timeout_ms;
    memcpy(proxy->backends, config->backends, proxy->count * sizeof(*proxy->backends));
    error = pthread_mutex_init(&proxy->mutex, NULL);
    if (error != 0) errno = error;
    return error == 0 ? 0 : -1;
}

void proxy_destroy(reverse_proxy *proxy)
{
    pthread_mutex_destroy(&proxy->mutex);
}

int proxy_next_backend(reverse_proxy *proxy, backend_config *backend)
{
    int result = -1;

    pthread_mutex_lock(&proxy->mutex);
    if (proxy->count != 0) {
        *backend = proxy->backends[proxy->next];
        proxy->next = (proxy->next + 1) % proxy->count;
        result = 0;
    }
    pthread_mutex_unlock(&proxy->mutex);
    return result;
}

static bool token_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static bool valid_token(const char *text)
{
    if (*text == '\0') return false;
    for (; *text; ++text) if (!token_char((unsigned char)*text)) return false;
    return true;
}

static bool valid_connection(const char *value)
{
    for (;;) {
        size_t count = 0;

        while (*value == ' ' || *value == '\t') ++value;
        while (token_char((unsigned char)*value)) { ++value; ++count; }
        if (count == 0) return false;
        while (*value == ' ' || *value == '\t') ++value;
        if (*value == '\0') return true;
        if (*value++ != ',') return false;
    }
}

static bool connection_names(const http_header *headers, size_t count, const char *name)
{
    size_t length = strlen(name);

    for (size_t i = 0; i < count; ++i) {
        const char *cursor;

        if (strcasecmp(headers[i].name, "Connection") != 0) continue;
        cursor = headers[i].value;
        while (*cursor) {
            const char *start;
            size_t size;

            while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
            start = cursor;
            while (*cursor && *cursor != ',') ++cursor;
            size = (size_t)(cursor - start);
            while (size && (start[size - 1] == ' ' || start[size - 1] == '\t')) --size;
            if (size == length && strncasecmp(start, name, length) == 0) return true;
        }
    }
    return false;
}

static bool hop_header(const char *name)
{
    static const char *const names[] = {
        "Connection", "Proxy-Connection", "Keep-Alive", "TE", "Trailer",
        "Transfer-Encoding", "Upgrade", "Proxy-Authorization", "Proxy-Authenticate"
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcasecmp(name, names[i]) == 0) return true;
    }
    return false;
}

static int append(char *wire, size_t capacity, size_t *used, const char *format, ...)
{
    va_list arguments;
    int length;

    va_start(arguments, format);
    length = vsnprintf(wire + *used, capacity - *used, format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= capacity - *used) {
        errno = EOVERFLOW;
        return -1;
    }
    *used += (size_t)length;
    return 0;
}

static int request_headers(int fd, const backend_config *backend, const http_request *request)
{
    char wire[PROXY_WIRE_BYTES];
    size_t used = 0;

    if (append(wire, sizeof(wire), &used, "%s %s HTTP/1.1\r\nHost: %s:%u\r\n",
               request->method, request->target, backend->host, (unsigned)backend->port) == -1) return -1;
    for (size_t i = 0; i < request->header_count; ++i) {
        const http_header *header = &request->headers[i];

        if (hop_header(header->name) || connection_names(request->headers, request->header_count, header->name) ||
            strcasecmp(header->name, "Host") == 0 || strcasecmp(header->name, "Content-Length") == 0) continue;
        if (append(wire, sizeof(wire), &used, "%s: %s\r\n", header->name, header->value) == -1) return -1;
    }
    if (request->has_content_length && append(wire, sizeof(wire), &used,
            "Content-Length: %zu\r\n", request->body_length) == -1) return -1;
    if (append(wire, sizeof(wire), &used, "Via: 1.1 c-webserver\r\nConnection: close\r\n\r\n") == -1) return -1;
    return http_send_all(fd, wire, used);
}

static bool decimal_length(const char *text, size_t *value)
{
    size_t result = 0;

    if (*text == '\0') return false;
    for (; *text; ++text) {
        unsigned digit = (unsigned char)*text - (unsigned)'0';

        if (digit > 9 || result > (SIZE_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

static int split_field(char *line, http_header *header)
{
    char *colon = strchr(line, ':');
    char *end;

    if (colon == NULL) return 502;
    *colon++ = '\0';
    if (!valid_token(line)) return 502;
    for (const unsigned char *p = (unsigned char *)colon; *p; ++p) {
        if ((*p < 0x20 && *p != '\t') || *p == 0x7f) return 502;
    }
    while (*colon == ' ' || *colon == '\t') ++colon;
    end = colon + strlen(colon);
    while (end > colon && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    *header = (http_header){line, colon};
    return 0;
}

int proxy_parse_response(const unsigned char *data, size_t length, proxy_response *response)
{
    char *cursor;
    char *end;
    bool transfer_encoding = false;

    memset(response, 0, sizeof(*response));
    if (data == NULL || length < 4 || length > HTTP_MAX_HEADER_BYTES ||
        memchr(data, '\0', length) != NULL || memcmp(data + length - 4, "\r\n\r\n", 4) != 0) return 502;
    memcpy(response->storage, data, length);
    response->storage[length] = '\0';
    cursor = response->storage;
    end = strstr(cursor, "\r\n");
    if (end == NULL) return 502;
    *end = '\0';
    if (strlen(cursor) < 13 || (strncmp(cursor, "HTTP/1.0 ", 9) != 0 && strncmp(cursor, "HTTP/1.1 ", 9) != 0) ||
        cursor[9] < '1' || cursor[9] > '5' || cursor[10] < '0' || cursor[10] > '9' ||
        cursor[11] < '0' || cursor[11] > '9' || cursor[12] != ' ') return 502;
    for (const unsigned char *p = (unsigned char *)cursor + 13; *p; ++p) {
        if ((*p < 0x20 && *p != '\t') || *p == 0x7f) return 502;
    }
    response->status_line = cursor;
    response->status = (cursor[9] - '0') * 100 + (cursor[10] - '0') * 10 + cursor[11] - '0';
    cursor = end + 2;
    while (strcmp(cursor, "\r\n") != 0) {
        http_header *header;

        end = strstr(cursor, "\r\n");
        if (end == NULL || response->header_count == HTTP_MAX_HEADERS) return 502;
        *end = '\0';
        header = &response->headers[response->header_count++];
        if (split_field(cursor, header) != 0) return 502;
        if (strcasecmp(header->name, "Content-Length") == 0) {
            if (response->has_length || !decimal_length(header->value, &response->length)) return 502;
            response->has_length = true;
        } else if (strcasecmp(header->name, "Transfer-Encoding") == 0) {
            if (transfer_encoding || strcasecmp(header->value, "chunked") != 0) return 502;
            transfer_encoding = response->chunked = true;
        } else if (strcasecmp(header->name, "Connection") == 0 && !valid_connection(header->value)) return 502;
        cursor = end + 2;
    }
    if (response->has_length && transfer_encoding) return 502;
    if ((response->status < 200 || response->status == 204) &&
        (response->has_length || transfer_encoding)) return 502;
    return 0;
}

static int response_headers(int client_fd, const proxy_response *response, bool keep_alive)
{
    char wire[PROXY_WIRE_BYTES];
    size_t used = 0;

    if (append(wire, sizeof(wire), &used, "HTTP/1.1 %s\r\n", response->status_line + 9) == -1) return -1;
    for (size_t i = 0; i < response->header_count; ++i) {
        const http_header *header = &response->headers[i];

        if (hop_header(header->name) || connection_names(response->headers, response->header_count, header->name) ||
            strcasecmp(header->name, "Content-Length") == 0) continue;
        if (append(wire, sizeof(wire), &used, "%s: %s\r\n", header->name, header->value) == -1) return -1;
    }
    if (response->has_length && append(wire, sizeof(wire), &used,
            "Content-Length: %zu\r\n", response->length) == -1) return -1;
    if (append(wire, sizeof(wire), &used, "Via: 1.1 c-webserver\r\nConnection: %s\r\n\r\n",
               keep_alive ? "keep-alive" : "close") == -1) return -1;
    return http_send_all(client_fd, wire, used);
}

static int io_status(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? 504 : 502;
}

static bool stopping(thread_pool *workers)
{
    return workers != NULL && thread_pool_is_stopping(workers);
}

static int64_t now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int connect_backend(int fd, const backend_config *backend, unsigned timeout, thread_pool *workers)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(backend->port)};
    int flags = fcntl(fd, F_GETFL);
    int64_t started = now_ms();

    if (started < 0 || flags == -1 || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 ||
        inet_pton(AF_INET, backend->host, &address.sin_addr) != 1) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        if (errno != EINPROGRESS) return -1;
        for (;;) {
            struct pollfd event = {.fd = fd, .events = POLLOUT};
            int64_t current = now_ms();
            int64_t remaining;
            int ready;
            int error = 0;
            socklen_t size = sizeof(error);

            if (stopping(workers)) { errno = ECANCELED; return -1; }
            if (current < 0) return -1;
            remaining = timeout - (current - started);
            if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
            /* 연결이 아직 성립하지 않아 shutdown이 깨우지 못하는 경우에도 종료를 확인한다. */
            ready = poll(&event, 1, remaining < 100 ? (int)remaining : 100);
            if (ready == -1 && errno == EINTR) continue;
            if (ready == -1) return -1;
            if (ready == 0) continue;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == -1) return -1;
            if (error != 0) { errno = error; return -1; }
            break;
        }
    }
    if (stopping(workers)) { errno = ECANCELED; return -1; }
    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) return -1;
    return http_set_socket_timeout(fd, timeout);
}

static ssize_t receive(int fd, void *data, size_t size)
{
    ssize_t count;

    do { count = recv(fd, data, size, 0); } while (count == -1 && errno == EINTR);
    return count;
}

static size_t header_end(const unsigned char *data, size_t length)
{
    for (size_t i = 0; i + 3 < length; ++i) {
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) return i + 4;
    }
    return 0;
}

typedef struct {
    int fd;
    unsigned char bytes[PROXY_CHUNK_BYTES];
    size_t used;
    size_t cursor;
} upstream_reader;

static int read_headers(upstream_reader *reader, proxy_response *response)
{
    for (unsigned informational = 0; informational <= 8; ++informational) {
        size_t length;

        for (;;) {
            ssize_t count;

            length = header_end(reader->bytes, reader->used);
            if (length != 0) break;
            if (reader->used == sizeof(reader->bytes)) return 502;
            count = receive(reader->fd, reader->bytes + reader->used, sizeof(reader->bytes) - reader->used);
            if (count < 0) return io_status();
            if (count == 0) return 502;
            reader->used += (size_t)count;
        }
        if (proxy_parse_response(reader->bytes, length, response) != 0) return 502;
        if (response->status == 101) return 502;
        reader->cursor = length;
        if (response->status >= 200) return 0;
        reader->used -= length;
        memmove(reader->bytes, reader->bytes + length, reader->used);
    }
    return 502;
}

static int read_byte(upstream_reader *reader, unsigned char *byte)
{
    if (reader->cursor == reader->used) {
        ssize_t count = receive(reader->fd, reader->bytes, sizeof(reader->bytes));

        if (count <= 0) return -1;
        reader->used = (size_t)count;
        reader->cursor = 0;
    }
    *byte = reader->bytes[reader->cursor++];
    return 0;
}

static int read_line(upstream_reader *reader, char *line, size_t capacity)
{
    size_t used = 0;

    for (;;) {
        unsigned char byte;

        if (read_byte(reader, &byte) == -1) return -1;
        if (byte == '\r') {
            if (read_byte(reader, &byte) == -1 || byte != '\n') return -1;
            line[used] = '\0';
            return 0;
        }
        if ((byte < 0x20 && byte != '\t') || byte == 0x7f || used + 1 >= capacity) return -1;
        line[used++] = (char)byte;
    }
}

static int transfer_exact(upstream_reader *reader, int client_fd, size_t length)
{
    while (length != 0) {
        size_t amount;

        if (reader->cursor == reader->used) {
            size_t wanted = length < sizeof(reader->bytes) ? length : sizeof(reader->bytes);
            ssize_t count = receive(reader->fd, reader->bytes, wanted);

            if (count <= 0) return -1;
            reader->cursor = 0;
            reader->used = (size_t)count;
        }
        amount = reader->used - reader->cursor;
        if (amount > length) amount = length;
        if (http_send_all(client_fd, reader->bytes + reader->cursor, amount) == -1) return -1;
        reader->cursor += amount;
        length -= amount;
    }
    return 0;
}

static int transfer_chunked(upstream_reader *reader, int client_fd)
{
    char line[1024];

    for (;;) {
        size_t length = 0;
        size_t digits = 0;
        unsigned char byte;

        if (read_line(reader, line, sizeof(line)) == -1) return -1;
        while (line[digits] && line[digits] != ';') {
            unsigned char c = (unsigned char)line[digits++];
            unsigned value;

            if (c >= '0' && c <= '9') value = c - '0';
            else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
            else return -1;
            if (length > (SIZE_MAX - value) / 16) return -1;
            length = length * 16 + value;
        }
        if (digits == 0) return -1;
        if (length == 0) {
            size_t trailer_bytes = 0;
            size_t trailer_count = 0;

            for (;;) {
                http_header trailer;

                if (read_line(reader, line, sizeof(line)) == -1) return -1;
                trailer_bytes += strlen(line) + 2;
                if (trailer_bytes > HTTP_MAX_HEADER_BYTES) return -1;
                if (line[0] == '\0') return 0;
                if (++trailer_count > HTTP_MAX_HEADERS || split_field(line, &trailer) != 0) return -1;
            }
        }
        if (transfer_exact(reader, client_fd, length) == -1 ||
            read_byte(reader, &byte) == -1 || byte != '\r' ||
            read_byte(reader, &byte) == -1 || byte != '\n') return -1;
    }
}

static int transfer_body(upstream_reader *reader, int client_fd, const proxy_response *response)
{
    if (response->status == 204 || response->status == 304) return 0;
    if (response->chunked) return transfer_chunked(reader, client_fd);
    if (response->has_length) return transfer_exact(reader, client_fd, response->length);
    for (;;) {
        ssize_t count;

        if (reader->used > reader->cursor &&
            http_send_all(client_fd, reader->bytes + reader->cursor, reader->used - reader->cursor) == -1) return -1;
        count = receive(reader->fd, reader->bytes, sizeof(reader->bytes));
        if (count <= 0) return count == 0 ? 0 : -1;
        reader->used = (size_t)count;
        reader->cursor = 0;
    }
}

int proxy_serve(reverse_proxy *proxy, int client_fd, http_request *request, thread_pool *workers)
{
    backend_config backend;
    upstream_reader reader = {0};
    proxy_response response;
    int fd;
    int status = 0;
    int result = -1;
    /* 백엔드 연결은 요청마다 닫는다. 프런트 연결의 재사용 여부는 별개다. */
    bool requested = request->keep_alive && strcmp(request->version, "HTTP/1.1") == 0;

    request->keep_alive = false;

    if (stopping(workers)) return -1;
    if (request->body_length != request->content_length ||
        (request->body_length && request->body == NULL)) return http_send_error(client_fd, 400);
    if (proxy_next_backend(proxy, &backend) == -1) return http_send_error(client_fd, 503);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return http_send_error(client_fd, 502);
    if (workers != NULL && thread_pool_attach_backend(workers, client_fd, fd) == -1) {
        close(fd);
        return -1;
    }
    if (connect_backend(fd, &backend, proxy->timeout_ms, workers) == -1 ||
        request_headers(fd, &backend, request) == -1 ||
        http_send_all(fd, request->body, request->body_length) == -1) {
        status = io_status();
        goto done;
    }
    reader.fd = fd;
    status = read_headers(&reader, &response);
    if (status != 0) goto done;
    if (stopping(workers)) goto done;
    request->keep_alive = requested &&
        (response.has_length || response.status == 204 || response.status == 304);
    /* 이 시점부터 실패하면 상태를 바꿔 덧붙이지 않고 연결을 닫는다. */
    if (response_headers(client_fd, &response, request->keep_alive) == -1) goto done;
    result = transfer_body(&reader, client_fd, &response);

done:
    if (workers != NULL) thread_pool_close_backend(workers, client_fd, fd);
    else close(fd);
    if (status != 0 && !stopping(workers)) result = http_send_error(client_fd, status);
    return result;
}
