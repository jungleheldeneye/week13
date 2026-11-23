#ifndef WEBSERVER_HTTP_H
#define WEBSERVER_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* 초기 구현값이다. 서버 설정 모듈을 연결할 때 조정할 수 있다. */
#define HTTP_MAX_HEADER_BYTES (16 * 1024)
#define HTTP_MAX_URI_BYTES 2048
#define HTTP_MAX_HEADERS 100
#define HTTP_MAX_BODY_BYTES (1024 * 1024)
#define HTTP_IDLE_TIMEOUT_MS 30000

enum {
    HTTP_OK = 0,
    HTTP_CONNECTION_CLOSED = -1,
    HTTP_IO_ERROR = -2
};

typedef struct {
    const char *name;
    const char *value;
} http_header;

typedef struct {
    char method[5];
    char target[HTTP_MAX_URI_BYTES + 1];
    char version[9];
    http_header headers[HTTP_MAX_HEADERS];
    size_t header_count;
    const char *host;
    bool has_content_length;
    size_t content_length;
    bool keep_alive;
    unsigned char *body;
    size_t body_length;
    /* headers와 host가 가리키는 메모리도 이 구조체가 소유한다. */
    char *header_storage;
} http_request;

typedef struct {
    unsigned char buffer[HTTP_MAX_HEADER_BYTES];
    size_t used;
} http_reader;

typedef struct {
    int status;
    const char *content_type;
    off_t content_length;
    const char *allow;
} http_response;

void http_request_init(http_request *request);
void http_request_free(http_request *request);
const char *http_get_header(const http_request *request, const char *name);

/* 초기화된 request에 헤더를 설정한다. 성공은 0, 실패는 HTTP 상태 코드. */
int http_parse_headers(const char *data, size_t length, http_request *request);

void http_reader_init(http_reader *reader);
int http_set_socket_timeout(int fd, unsigned milliseconds);
/* 연결별 reader에 미리 읽은 다음 요청의 바이트를 보존한다.
 * 반환값은 0, 양수 HTTP 오류 상태, 또는 위의 음수 연결/I/O 상태다.
 * 성공한 요청은 호출자가 사용 후 해제하고, 실패 시에는 내부에서 해제한다.
 */
int http_read_request(int fd, http_reader *reader, http_request *request);

const char *http_status_reason(int status);
int http_send_all(int fd, const void *buffer, size_t length);
int http_send_headers(int fd, const http_response *response);
int http_send_error(int fd, int status);
int http_send_error_with_allow(int fd, int status, const char *allow);
/* 큐가 찼을 때 대기 없이 503을 시도한다. fd를 논블로킹으로 바꾸며 호출자가 닫는다. */
int http_send_unavailable(int fd);

#endif
