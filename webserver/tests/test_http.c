#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static unsigned parser_cases;

static void check_parse(const char *text, int expected)
{
    http_request request;
    int status;

    http_request_init(&request);
    status = http_parse_headers(text, strlen(text), &request);
    if (status != expected) {
        fprintf(stderr, "expected %d, got %d: %s\n", expected, status, text);
    }
    CHECK(status == expected);
    if (status != HTTP_OK) {
        CHECK(request.header_storage == NULL && request.body == NULL);
    }
    http_request_free(&request);
    ++parser_cases;
}

static void test_parser(void)
{
    static const struct {
        const char *text;
        int status;
    } cases[] = {
        {"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", HTTP_OK},
        {"GET /a?q=1 HTTP/1.0\r\n\r\n", HTTP_OK},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", HTTP_OK},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 00011\r\n\r\n", HTTP_OK},
        {"POST / HTTP/1.1\r\nHost: x\r\n\r\n", 411},
        {"GET / HTTP/1.1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost:\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a,b\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a b\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nNo-colon\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\n folded: x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nX: a\001b\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nX: a\177b\r\n\r\n", 400},
        {"GET / HTTP/1.1\nHost: x\n\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\n\r\nExtra: x\r\n\r\n", 400},
        {"GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"G@T / HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"PUT / HTTP/1.1\r\nHost: x\r\n\r\n", 405},
        {"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", 405},
        {"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505},
        {"GET / HTTP/1.x\r\nHost: x\r\n\r\n", 400},
        {"GET http://x/ HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET /a#b HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1 1\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1,1\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999999999999999999999\r\n\r\n", 413},
        {"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        {"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding:\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nExpect: 100-continue\r\n\r\n", 417},
        {"GET / HTTP/1.1\r\nHost: x\r\nConnection: close,\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nConnection: keep alive\r\n\r\n", 400}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        check_parse(cases[i].text, cases[i].status);
    }
}

static void test_metadata_and_limits(void)
{
    http_request request;
    const char *text = "POST /echo?q=1 HTTP/1.1\r\nhOsT: localhost:8080\r\n"
                       "content-length:\t11 \t\r\nX-Test: value\r\n"
                       "Connection: Keep-Alive\r\nConnection: CLOSE\r\n\r\n";
    char length_header[128];
    char *large = malloc(HTTP_MAX_HEADER_BYTES + 2);
    const char *prefix = "GET / HTTP/1.1\r\nHost: x\r\nX-Pad: ";
    const char bad_header[] = "GET / HTTP/1.1\r\nHost: x\0hidden\r\n\r\n";
    size_t used;

    CHECK(large != NULL);
    http_request_init(&request);
    CHECK(http_parse_headers(text, strlen(text), &request) == HTTP_OK);
    CHECK(strcmp(request.method, "POST") == 0);
    CHECK(strcmp(request.target, "/echo?q=1") == 0);
    CHECK(strcmp(request.host, "localhost:8080") == 0);
    CHECK(strcmp(http_get_header(&request, "x-test"), "value") == 0);
    CHECK(http_get_header(&request, "missing") == NULL);
    CHECK(request.has_content_length && request.content_length == 11);
    CHECK(!request.keep_alive);
    CHECK(request.body == NULL && request.body_length == 0);
    http_request_free(&request);

    text = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(http_parse_headers(text, strlen(text), &request) == HTTP_OK);
    CHECK(request.keep_alive && !request.has_content_length);
    text = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    CHECK(http_parse_headers(text, strlen(text), &request) == HTTP_OK);
    CHECK(request.keep_alive);
    text = "GET / HTTP/1.0\r\n\r\n";
    CHECK(http_parse_headers(text, strlen(text), &request) == HTTP_OK);
    CHECK(!request.keep_alive);
    http_request_free(&request);

    snprintf(length_header, sizeof(length_header),
             "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %u\r\n\r\n",
             (unsigned)HTTP_MAX_BODY_BYTES);
    check_parse(length_header, HTTP_OK);
    snprintf(length_header, sizeof(length_header),
             "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %u\r\n\r\n",
             (unsigned)HTTP_MAX_BODY_BYTES + 1);
    check_parse(length_header, 413);

    memset(large, 'a', HTTP_MAX_HEADER_BYTES);
    memcpy(large, prefix, strlen(prefix));
    memcpy(large + HTTP_MAX_HEADER_BYTES - 4, "\r\n\r\n", 5);
    check_parse(large, HTTP_OK);
    CHECK(http_parse_headers(large, HTTP_MAX_HEADER_BYTES + 1, &request) == 431);

    used = (size_t)snprintf(large, HTTP_MAX_HEADER_BYTES, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (size_t i = 1; i < HTTP_MAX_HEADERS; ++i) {
        used += (size_t)snprintf(large + used, HTTP_MAX_HEADER_BYTES - used,
                                 "X-%zu: a\r\n", i);
    }
    strcpy(large + used, "\r\n");
    check_parse(large, HTTP_OK);
    strcpy(large + used, "X-Extra: a\r\n\r\n");
    check_parse(large, 431);

    memcpy(large, "GET /", 5);
    memset(large + 5, 'a', HTTP_MAX_URI_BYTES - 1);
    strcpy(large + 4 + HTTP_MAX_URI_BYTES, " HTTP/1.1\r\nHost: x\r\n\r\n");
    check_parse(large, HTTP_OK);
    large[4 + HTTP_MAX_URI_BYTES] = 'a';
    strcpy(large + 5 + HTTP_MAX_URI_BYTES, " HTTP/1.1\r\nHost: x\r\n\r\n");
    check_parse(large, 414);

    CHECK(http_parse_headers(bad_header, sizeof(bad_header) - 1, &request) == 400);
    CHECK(http_parse_headers(NULL, 0, &request) == 400);
    free(large);
    http_request_free(&request);
}

static void make_pair(int sockets[2], unsigned timeout_ms)
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(http_set_socket_timeout(sockets[0], timeout_ms) == 0);
    CHECK(http_set_socket_timeout(sockets[1], timeout_ms) == 0);
}

static void write_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t offset = 0;

    while (offset < length) {
        ssize_t count = send(fd, bytes + offset, length - offset, 0);
        if (count == -1 && errno == EINTR) {
            continue;
        }
        CHECK(count > 0);
        offset += (size_t)count;
    }
}

typedef struct {
    int fd;
    const unsigned char *data;
    size_t length;
    size_t chunk_size;
    long delay_ns;
} writer_job;

static void *fragment_writer(void *argument)
{
    writer_job *job = argument;

    for (size_t offset = 0; offset < job->length;) {
        size_t chunk = job->length - offset;
        struct timespec delay = {0, job->delay_ns};

        if (chunk > job->chunk_size) {
            chunk = job->chunk_size;
        }
        write_all(job->fd, job->data + offset, chunk);
        offset += chunk;
        if (offset != job->length && job->delay_ns != 0) {
            while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {
            }
        }
    }
    CHECK(shutdown(job->fd, SHUT_WR) == 0);
    return NULL;
}

static void test_fragmented_request(void)
{
    const unsigned char wire[] = "POST /echo HTTP/1.1\r\nHost: x\r\n"
                                 "Content-Length: 11\r\n\r\nhello world";
    int sockets[2];
    http_reader reader;
    http_request request;
    pthread_t thread;
    writer_job job;

    make_pair(sockets, 1000);
    http_reader_init(&reader);
    http_request_init(&request);
    job = (writer_job){sockets[1], wire, sizeof(wire) - 1, 1, 1000000};
    CHECK(pthread_create(&thread, NULL, fragment_writer, &job) == 0);
    CHECK(http_read_request(sockets[0], &reader, &request) == HTTP_OK);
    CHECK(request.body_length == 11);
    CHECK(memcmp(request.body, "hello world", 11) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    http_request_free(&request);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_binary_and_next_request(void)
{
    const char header[] = "POST /binary HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\n";
    const unsigned char body[] = {'a', 0, 'b'};
    const char next[] = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    unsigned char wire[sizeof(header) - 1 + sizeof(body) + sizeof(next) - 1];
    int sockets[2];
    http_reader reader;
    http_request request;

    memcpy(wire, header, sizeof(header) - 1);
    memcpy(wire + sizeof(header) - 1, body, sizeof(body));
    memcpy(wire + sizeof(header) - 1 + sizeof(body), next, sizeof(next) - 1);
    make_pair(sockets, 1000);
    write_all(sockets[1], wire, sizeof(wire));
    CHECK(shutdown(sockets[1], SHUT_WR) == 0);
    http_reader_init(&reader);
    http_request_init(&request);
    CHECK(http_read_request(sockets[0], &reader, &request) == HTTP_OK);
    CHECK(request.body_length == 3 && memcmp(request.body, body, 3) == 0);
    CHECK(http_read_request(sockets[0], &reader, &request) == HTTP_OK);
    CHECK(strcmp(request.target, "/next") == 0 && request.body == NULL);
    CHECK(http_read_request(sockets[0], &reader, &request) == HTTP_CONNECTION_CLOSED);
    http_request_free(&request);
    close(sockets[0]);
    close(sockets[1]);
}

static void check_read_failure(const char *wire, bool close_write, int expected)
{
    int sockets[2];
    http_reader reader;
    http_request request;

    make_pair(sockets, 30);
    write_all(sockets[1], wire, strlen(wire));
    if (close_write) {
        CHECK(shutdown(sockets[1], SHUT_WR) == 0);
    }
    http_reader_init(&reader);
    http_request_init(&request);
    CHECK(http_read_request(sockets[0], &reader, &request) == expected);
    CHECK(request.body == NULL && request.header_storage == NULL);
    CHECK(request.body_length == 0 && reader.used == 0);
    http_request_free(&request);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_eof_and_idle_timeout(void)
{
    check_read_failure("", true, HTTP_CONNECTION_CLOSED);
    check_read_failure("GET / HTTP/1.1\r\nHost: x\r\n", true, 400);
    check_read_failure("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\nhello", true, 400);
    check_read_failure("", false, 408);
    check_read_failure("GET / HTTP/1.1\r\nHost: x\r\n", false, 408);
    check_read_failure("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\nhello", false, 408);
    check_read_failure("POST / HTTP/1.1\r\nHost: x\r\n\r\n", false, 411);
}

static void test_progress_resets_idle_timeout(void)
{
    const char header[] = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\n";
    const unsigned char body[] = "1234567";
    int sockets[2];
    pthread_t thread;
    writer_job job;
    http_reader reader;
    http_request request;

    make_pair(sockets, 200);
    write_all(sockets[1], header, sizeof(header) - 1);
    job = (writer_job){sockets[1], body, sizeof(body) - 1, 1, 60000000};
    CHECK(pthread_create(&thread, NULL, fragment_writer, &job) == 0);
    http_reader_init(&reader);
    http_request_init(&request);
    /* 전체 수신은 약 360ms이지만 각 무응답 구간은 200ms보다 짧다. */
    CHECK(http_read_request(sockets[0], &reader, &request) == HTTP_OK);
    CHECK(request.body_length == 7 && memcmp(request.body, body, 7) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    http_request_free(&request);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_reader_header_limit(void)
{
    char *wire = malloc(HTTP_MAX_HEADER_BYTES + 1);
    const char *prefix = "GET / HTTP/1.1\r\nHost: x\r\nX-Pad: ";
    int sockets[2];
    http_reader reader;
    http_request request;
    pthread_t thread;
    writer_job job;

    CHECK(wire != NULL);
    memset(wire, 'a', HTTP_MAX_HEADER_BYTES);
    memcpy(wire, prefix, strlen(prefix));
    memcpy(wire + HTTP_MAX_HEADER_BYTES - 4, "\r\n\r\n", 4);
    make_pair(sockets, 1000);
    job = (writer_job){sockets[1], (const unsigned char *)wire,
                       HTTP_MAX_HEADER_BYTES, 1024, 0};
    CHECK(pthread_create(&thread, NULL, fragment_writer, &job) == 0);
    http_reader_init(&reader);
    http_request_init(&request);
    CHECK(http_read_request(sockets[0], &reader, &request) == HTTP_OK);
    CHECK(pthread_join(thread, NULL) == 0);
    http_request_free(&request);
    close(sockets[0]);
    close(sockets[1]);
    memset(wire, 'a', HTTP_MAX_HEADER_BYTES);
    wire[HTTP_MAX_HEADER_BYTES] = '\0';
    make_pair(sockets, 1000);
    job.fd = sockets[1];
    CHECK(pthread_create(&thread, NULL, fragment_writer, &job) == 0);
    http_reader_init(&reader);
    CHECK(http_read_request(sockets[0], &reader, &request) == 431);
    CHECK(pthread_join(thread, NULL) == 0);
    http_request_free(&request);
    close(sockets[0]);
    close(sockets[1]);
    free(wire);
}

static void test_error_response(void)
{
    int sockets[2];
    char response[1024];
    size_t used = 0;
    char *body;
    char *length;

    make_pair(sockets, 1000);
    CHECK(http_send_error(sockets[0], 411) == 0);
    CHECK(shutdown(sockets[0], SHUT_WR) == 0);
    for (;;) {
        ssize_t count = recv(sockets[1], response + used, sizeof(response) - 1 - used, 0);
        CHECK(count >= 0);
        if (count == 0) {
            break;
        }
        used += (size_t)count;
        CHECK(used < sizeof(response) - 1);
    }
    response[used] = '\0';
    CHECK(strncmp(response, "HTTP/1.1 411 Length Required\r\n", 30) == 0);
    CHECK(strstr(response, "Connection: close\r\n") != NULL);
    body = strstr(response, "\r\n\r\n");
    length = strstr(response, "Content-Length: ");
    CHECK(body != NULL && length != NULL);
    CHECK(strtoul(length + strlen("Content-Length: "), NULL, 10) == strlen(body + 4));
    close(sockets[0]);
    close(sockets[1]);

    make_pair(sockets, 1000);
    close(sockets[1]);
    CHECK(http_send_error(sockets[0], 400) == -1);
    close(sockets[0]);
}

static void test_unavailable_response(void)
{
    int sockets[2];
    char response[1024] = {0};
    char buffer[4096] = {0};
    size_t used = 0;
    int flags;
    struct timespec before, after;

    make_pair(sockets, 1000);
    CHECK(http_send_unavailable(sockets[0]) == 0);
    CHECK(shutdown(sockets[0], SHUT_WR) == 0);
    for (;;) {
        ssize_t count = recv(sockets[1], response + used, sizeof(response) - 1 - used, 0);

        CHECK(count >= 0);
        if (count == 0) break;
        used += (size_t)count;
        CHECK(used < sizeof(response) - 1);
    }
    CHECK(strstr(response, "HTTP/1.1 503 Service Unavailable\r\n") == response);
    CHECK(strstr(response, "Content-Length: 24\r\n") != NULL);
    CHECK(strstr(response, "\r\n\r\n503 Service Unavailable\n") != NULL);
    close(sockets[0]);
    close(sockets[1]);

    make_pair(sockets, 1000);
    flags = fcntl(sockets[0], F_GETFL);
    CHECK(flags != -1 && fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);
    while (send(sockets[0], buffer, sizeof(buffer), 0) > 0) {}
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    CHECK(fcntl(sockets[0], F_SETFL, flags) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    CHECK(http_send_unavailable(sockets[0]) == -1);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    CHECK((after.tv_sec - before.tv_sec) + (after.tv_nsec - before.tv_nsec) / 1e9 < 0.5);
    close(sockets[0]);
    close(sockets[1]);
}

int main(void)
{
    CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    test_parser();
    test_metadata_and_limits();
    test_fragmented_request();
    test_binary_and_next_request();
    test_eof_and_idle_timeout();
    test_progress_resets_idle_timeout();
    test_reader_header_limit();
    test_error_response();
    test_unavailable_response();
    printf("PASS: %u parser cases, metadata and size boundaries, fragmented/binary "
           "bodies, next-request preservation, EOF, idle timeout, error responses\n",
           parser_cases);
    return EXIT_SUCCESS;
}
