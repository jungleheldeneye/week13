#include "http.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void http_request_init(http_request *request)
{
    memset(request, 0, sizeof(*request));
}

void http_request_free(http_request *request)
{
    free(request->body);
    free(request->header_storage);
    http_request_init(request);
}

const char *http_get_header(const http_request *request, const char *name)
{
    for (size_t i = 0; i < request->header_count; ++i) {
        if (strcasecmp(request->headers[i].name, name) == 0) {
            return request->headers[i].value;
        }
    }
    return NULL;
}

static bool is_token_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static bool is_token(const char *text)
{
    if (*text == '\0') {
        return false;
    }
    for (; *text != '\0'; ++text) {
        if (!is_token_char((unsigned char)*text)) {
            return false;
        }
    }
    return true;
}

static int parse_request_line(char *line, http_request *request)
{
    char *target = strchr(line, ' ');
    char *version;

    if (target == NULL) {
        return 400;
    }
    *target++ = '\0';
    version = strchr(target, ' ');
    if (version == NULL) {
        return 400;
    }
    *version++ = '\0';
    if (!is_token(line) || *target != '/' || strchr(version, ' ') != NULL) {
        return 400;
    }
    if (strlen(target) > HTTP_MAX_URI_BYTES) {
        return 414;
    }
    for (const unsigned char *p = (const unsigned char *)target; *p; ++p) {
        if (*p < 0x21 || *p > 0x7e || *p == '#') {
            return 400;
        }
    }
    if (strcmp(version, "HTTP/1.0") != 0 && strcmp(version, "HTTP/1.1") != 0) {
        if (strlen(version) == 8 && strncmp(version, "HTTP/", 5) == 0 &&
            version[5] >= '0' && version[5] <= '9' && version[6] == '.' &&
            version[7] >= '0' && version[7] <= '9') {
            return 505;
        }
        return 400;
    }
    if (strcmp(line, "GET") != 0 && strcmp(line, "POST") != 0) {
        return 405;
    }

    strcpy(request->method, line);
    strcpy(request->target, target);
    strcpy(request->version, version);
    return HTTP_OK;
}

static int parse_length(const char *value, size_t *length)
{
    size_t result = 0;

    if (*value == '\0') {
        return 400;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        unsigned digit;

        if (*p < '0' || *p > '9') {
            return 400;
        }
        digit = *p - '0';
        if (result > (SIZE_MAX - digit) / 10) {
            return 413;
        }
        result = result * 10 + digit;
    }
    if (result > HTTP_MAX_BODY_BYTES) {
        return 413;
    }
    *length = result;
    return HTTP_OK;
}

static int parse_connection(const char *value, bool *close, bool *keep_alive)
{
    for (;;) {
        const char *end = strchr(value, ',');
        const char *last = end != NULL ? end : value + strlen(value);

        while (*value == ' ' || *value == '\t') {
            ++value;
        }
        while (last > value && (last[-1] == ' ' || last[-1] == '\t')) {
            --last;
        }
        if (last == value) {
            return 400;
        }
        for (const char *p = value; p < last; ++p) {
            if (!is_token_char((unsigned char)*p)) {
                return 400;
            }
        }
        if (last - value == 5 && strncasecmp(value, "close", 5) == 0) {
            *close = true;
        }
        if (last - value == 10 && strncasecmp(value, "keep-alive", 10) == 0) {
            *keep_alive = true;
        }
        if (end == NULL) {
            return HTTP_OK;
        }
        value = end + 1;
    }
}

static bool valid_host(const char *value)
{
    if (*value == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p <= 0x20 || *p >= 0x7f || strchr("/\\?#,@", *p) != NULL) {
            return false;
        }
    }
    return true;
}

static int parse_fields(char *cursor, http_request *request)
{
    bool transfer_encoding = false;
    bool expect = false;
    bool connection_close = false;
    bool connection_keep_alive = false;

    while (*cursor != '\r') {
        char *end = strstr(cursor, "\r\n");
        char *value;
        char *last;
        int status;

        if (end == NULL || request->header_count == HTTP_MAX_HEADERS) {
            return end == NULL ? 400 : 431;
        }
        *end = '\0';
        value = strchr(cursor, ':');
        if (value == NULL) {
            return 400;
        }
        *value++ = '\0';
        if (!is_token(cursor)) {
            return 400;
        }
        for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
            if ((*p < 0x20 && *p != '\t') || *p == 0x7f) {
                return 400;
            }
        }
        while (*value == ' ' || *value == '\t') {
            ++value;
        }
        last = value + strlen(value);
        while (last > value && (last[-1] == ' ' || last[-1] == '\t')) {
            *--last = '\0';
        }
        request->headers[request->header_count++] = (http_header){cursor, value};

        if (strcasecmp(cursor, "Host") == 0) {
            if (request->host != NULL || !valid_host(value)) {
                return 400;
            }
            request->host = value;
        } else if (strcasecmp(cursor, "Content-Length") == 0) {
            if (request->has_content_length) {
                return 400;
            }
            status = parse_length(value, &request->content_length);
            if (status != HTTP_OK) {
                return status;
            }
            request->has_content_length = true;
        } else if (strcasecmp(cursor, "Transfer-Encoding") == 0) {
            if (*value == '\0') {
                return 400;
            }
            transfer_encoding = true;
        } else if (strcasecmp(cursor, "Expect") == 0) {
            expect = true;
        } else if (strcasecmp(cursor, "Connection") == 0) {
            status = parse_connection(value, &connection_close, &connection_keep_alive);
            if (status != HTTP_OK) {
                return status;
            }
        }
        cursor = end + 2;
    }
    /* 헤더 종료 뒤의 다른 데이터를 헤더의 일부로 받아들이지 않는다. */
    if (strcmp(cursor, "\r\n") != 0) {
        return 400;
    }
    if (strcmp(request->version, "HTTP/1.1") == 0 && request->host == NULL) {
        return 400;
    }
    if (transfer_encoding) {
        return request->has_content_length ? 400 : 501;
    }
    if (expect) {
        return 417;
    }
    if (strcmp(request->method, "POST") == 0 && !request->has_content_length) {
        return 411;
    }
    request->keep_alive = !connection_close &&
        (strcmp(request->version, "HTTP/1.1") == 0 || connection_keep_alive);
    return HTTP_OK;
}

int http_parse_headers(const char *data, size_t length, http_request *request)
{
    char *end;
    int status;

    http_request_free(request);
    if (length > HTTP_MAX_HEADER_BYTES) {
        return 431;
    }
    if (data == NULL || length < 4 || memchr(data, '\0', length) != NULL ||
        memcmp(data + length - 4, "\r\n\r\n", 4) != 0) {
        return 400;
    }
    request->header_storage = malloc(length + 1);
    if (request->header_storage == NULL) {
        return 500;
    }
    memcpy(request->header_storage, data, length);
    request->header_storage[length] = '\0';
    end = strstr(request->header_storage, "\r\n");
    *end = '\0';
    status = parse_request_line(request->header_storage, request);
    if (status == HTTP_OK) {
        status = parse_fields(end + 2, request);
    }
    if (status != HTTP_OK) {
        http_request_free(request);
    }
    return status;
}
