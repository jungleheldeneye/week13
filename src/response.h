/*
 * response.h — HTTP 응답 만들기와 보내기
 *
 * 이 파일이 하는 일:
 *   상태 줄 + 공통 헤더(Date, Server, Content-Type, Content-Length, Connection)를 만들고
 *   본문과 함께 writev로 한 번에 보낸다. 오류 응답용 HTML 페이지와 MIME 표도 여기 있다.
 */
#ifndef RESPONSE_H
#define RESPONSE_H

#include <stddef.h>
#include <time.h>

#define SERVER_NAME "cweb/0.3"

/* 응답 하나를 보낼 때 필요한 정보 */
typedef struct {
    int         status;        /* 200, 404 ... */
    const char *content_type;  /* "text/html; charset=utf-8" 등, NULL이면 생략 */
    long long   content_length;/* 본문 전체 길이 (일부만 body로 넘겨도 됨) */
    const char *extra;         /* 추가 헤더 원문("Allow: GET\r\n" 등), 없으면 NULL */
    int         keep_alive;    /* 1이면 Connection: keep-alive */
    int         head_only;     /* HEAD 요청: 헤더만 보내고 본문은 생략 */
} resp_t;

/*
 * resp_send — 헤더와 본문 첫 조각을 한 번에 보낸다.
 *   입력: fd, r(응답 정보), body/blen(이번에 같이 보낼 본문 조각, 없으면 NULL/0)
 *   결과: 0 성공, -1 전송 실패(연결을 닫아야 함)
 *   본문이 더 남았다면 호출한 쪽이 이어서 net_write_all로 보낸다.
 */
int resp_send(int fd, const resp_t *r, const void *body, size_t blen);

/*
 * resp_error — 상태 코드에 맞는 작은 HTML 오류 페이지를 보낸다.
 *   입력: fd, 상태 코드, keep_alive, head_only, extra(추가 헤더, 없으면 NULL)
 *   결과: 보낸 본문 바이트 수(로그용), 실패 시 -1
 */
long long resp_error(int fd, int status, int keep_alive, int head_only, const char *extra);

/* mime_type — 파일 경로의 확장자로 Content-Type 결정. 모르면 application/octet-stream */
const char *mime_type(const char *path);

/* http_date — 시각을 HTTP 표준 날짜 문자열로 ("Wed, 25 Nov 2026 05:52:25 GMT") */
void http_date(time_t t, char *out, size_t outlen);

#endif
