#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

const char *http_status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
    }
}

int http_send_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *bytes = buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t count = send(fd, bytes + sent, length - sent, 0);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = EPIPE;
            }
            return -1;
        }
        sent += (size_t)count;
    }
    return 0;
}

int http_send_headers(int fd, const http_response *response)
{
    char header[512];
    char allow[128] = "";
    int length;

    if (response->content_type == NULL || response->content_length < 0 ||
        strpbrk(response->content_type, "\r\n") != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (response->allow != NULL) {
        if (strpbrk(response->allow, "\r\n") != NULL) {
            errno = EINVAL;
            return -1;
        }
        length = snprintf(allow, sizeof(allow), "Allow: %s\r\n", response->allow);
        if (length < 0 || (size_t)length >= sizeof(allow)) {
            errno = EOVERFLOW;
            return -1;
        }
    }
    length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %jd\r\n"
        "Connection: close\r\n"
        "%s\r\n",
        response->status, http_status_reason(response->status), response->content_type,
        (intmax_t)response->content_length, allow);
    if (length < 0 || (size_t)length >= sizeof(header)) {
        errno = EOVERFLOW;
        return -1;
    }
    return http_send_all(fd, header, (size_t)length);
}

int http_send_error_with_allow(int fd, int status, const char *allow)
{
    char body[96];
    int length = snprintf(body, sizeof(body), "%d %s\n", status, http_status_reason(status));
    http_response response = {
        .status = status,
        .content_type = "text/plain; charset=utf-8",
        .content_length = length,
        .allow = status == 405 ? allow : NULL
    };

    if (length < 0 || (size_t)length >= sizeof(body)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (http_send_headers(fd, &response) == -1) {
        return -1;
    }
    return http_send_all(fd, body, (size_t)length);
}

int http_send_error(int fd, int status)
{
    return http_send_error_with_allow(fd, status, "GET, POST");
}

int http_send_unavailable(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    return http_send_error(fd, 503);
}
