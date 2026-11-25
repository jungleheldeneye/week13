/*
 * conn.h — 연결 하나의 상태와 "요청 하나 처리"
 *
 * 이 파일이 하는 일:
 *   연결(소켓 + 수신 버퍼 + 처리한 요청 수 + 마지막 활동 시각)을 conn_t 하나로 묶고,
 *   conn_serve_one으로 "요청 1개 읽기 → 라우팅 → 응답"을 한다.
 *   - 스레드 풀(pool): 대기실에서 데이터가 온 연결만 꺼내 conn_serve_one을 부른다
 *   - thread/fork:    conn_run_blocking으로 연결이 끝날 때까지 반복해서 부른다
 */
#ifndef CONN_H
#define CONN_H

#include <time.h>
#include "config.h"
#include "net.h"

typedef struct conn {
    int          fd;          /* connected 소켓 */
    char         peer[64];    /* "127.0.0.1:50312" (로그용) */
    net_reader_t r;           /* 수신 버퍼: 연결과 수명이 같아야 미리 온 다음 요청을 잃지 않음 */
    int          served;      /* 이 연결에서 처리한 요청 수 */
    time_t       last;        /* 마지막 활동 시각 (대기실 시간 초과 판단용) */
} conn_t;

/* conn_new — accept한 fd로 연결 객체를 만든다. 결과: 객체, 메모리 부족이면 NULL */
conn_t *conn_new(int fd, const struct sockaddr *sa, socklen_t salen);

/* conn_free — 소켓을 닫고 메모리를 반납한다 */
void conn_free(conn_t *c);

/*
 * conn_serve_one — 요청 하나를 읽고 응답한다.
 *   결과: 1 = 연결 유지(keep-alive), 0 = 닫아야 함
 *   who: 로그에 찍을 처리 주체 ("w3", "pid 1234")
 */
int conn_serve_one(conn_t *c, const server_config_t *cfg, const char *who);

/* conn_run_blocking — 연결이 끝날 때까지 요청을 반복 처리하고 연결을 해제한다 (thread/fork용) */
void conn_run_blocking(conn_t *c, const server_config_t *cfg, const char *who);

#endif
