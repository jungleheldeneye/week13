#include "http.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

int http_wait_next_request(int fd, const http_reader *reader, unsigned milliseconds)
{
    struct pollfd event = {.fd = fd, .events = POLLIN};
    struct timespec now;
    int64_t deadline;

    if (reader->used != 0) return HTTP_OK;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return HTTP_IO_ERROR;
    deadline = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + milliseconds;
    for (;;) {
        int64_t remaining;
        int ready;

        if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return HTTP_IO_ERROR;
        remaining = deadline - ((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
        if (remaining <= 0) return HTTP_CONNECTION_CLOSED;
        ready = poll(&event, 1, remaining > INT32_MAX ? INT32_MAX : (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (event.revents & POLLNVAL)) return HTTP_IO_ERROR;
        if (ready > 0) return HTTP_OK; /* EOF・shutdown도 recv를 깨워 정리한다. */
    }
}

void http_reader_init(http_reader *reader)
{
    reader->used = 0;
}

int http_set_socket_timeout(int fd, unsigned milliseconds)
{
    struct timeval timeout;

    if (milliseconds == 0) {
        errno = EINVAL;
        return -1;
    }
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
        return -1;
    }
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static ssize_t receive(int fd, void *buffer, size_t capacity)
{
    ssize_t received;

    do {
        received = recv(fd, buffer, capacity, 0);
    } while (received == -1 && errno == EINTR);
    return received;
}

static int receive_error(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT
        ? 408 : HTTP_IO_ERROR;
}

static void consume(http_reader *reader, size_t count)
{
    reader->used -= count;
    memmove(reader->buffer, reader->buffer + count, reader->used);
}

static size_t header_end(const http_reader *reader, size_t start)
{
    for (size_t i = start; i + 3 < reader->used; ++i) {
        if (memcmp(reader->buffer + i, "\r\n\r\n", 4) == 0) {
            return i + 4;
        }
    }
    return 0;
}

int http_read_request(int fd, http_reader *reader, http_request *request)
{
    size_t header_length;
    size_t scan_from = 0;
    int status;

    http_request_free(request);
    for (;;) {
        ssize_t received;
        size_t previous_used;

        header_length = header_end(reader, scan_from);
        if (header_length != 0) {
            break;
        }
        if (reader->used == sizeof(reader->buffer)) {
            status = 431;
            goto failed;
        }
        previous_used = reader->used;
        received = receive(fd, reader->buffer + reader->used,
                           sizeof(reader->buffer) - reader->used);
        if (received <= 0) {
            status = received == 0
                ? (reader->used == 0 ? HTTP_CONNECTION_CLOSED : 400)
                : receive_error();
            goto failed;
        }
        reader->used += (size_t)received;
        /* 이전 recv와 이번 recv 사이에 걸친 구분자도 검사한다. */
        scan_from = previous_used > 3 ? previous_used - 3 : 0;
    }

    status = http_parse_headers((const char *)reader->buffer, header_length, request);
    if (status != HTTP_OK) {
        goto failed;
    }
    consume(reader, header_length);
    if (request->content_length != 0) {
        size_t buffered;

        request->body = malloc(request->content_length);
        if (request->body == NULL) {
            status = 500;
            goto failed;
        }
        buffered = reader->used < request->content_length
            ? reader->used : request->content_length;
        memcpy(request->body, reader->buffer, buffered);
        request->body_length = buffered;
        consume(reader, buffered);

        while (request->body_length < request->content_length) {
            ssize_t received = receive(fd, request->body + request->body_length,
                                      request->content_length - request->body_length);

            if (received <= 0) {
                status = received == 0 ? 400 : receive_error();
                goto failed;
            }
            request->body_length += (size_t)received;
        }
    }
    return HTTP_OK;

failed: {
    int saved_errno = errno;

    reader->used = 0;
    http_request_free(request);
    errno = saved_errno;
    return status;
}
}
