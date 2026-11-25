/*
 * server.h — 서버 본체 (연결 받기 → 처리 → 닫기 반복)
 *
 * 이 파일이 하는 일:
 *   listening 소켓을 열고, -m 옵션(pool/thread/fork)에 맞는 방식으로 여러 연결을 동시에 처리한다.
 *   한 연결에서는 keep-alive로 요청을 여러 번 처리할 수 있다.
 *   Ctrl+C(SIGINT)/SIGTERM을 받으면 진행 중인 연결을 마치고 깔끔하게 종료한다.
 */
#ifndef SERVER_H
#define SERVER_H

#include "config.h"

#define SERVER_BACKLOG   4096  /* listen 대기열 길이: accept 전 대기 가능한 연결 수.
                                  128일 때 동시 1000 연결에서 넘친 SYN이 버려져 1초 재전송 지연(p99 1초) 발생 → 4096.
                                  커널 상한(Linux somaxconn, macOS kern.ipc.somaxconn)을 넘으면 자동으로 깎임 */
#define SERVER_TIMEOUT_S   10   /* 첫 요청을 기다리는 시간과 읽기/쓰기 시간 상한(초) */
#define SERVER_KEEPALIVE_S  5   /* keep-alive: 다음 요청을 기다리는 시간(초) */
#define SERVER_KEEPALIVE_MAX 100 /* keep-alive: 연결 하나로 처리할 최대 요청 수 */
#define SERVER_REQUEST_DEADLINE_S 30 /* 요청 하나(헤더+본문)를 받는 전체 시간 상한(초) */

/*
 * server_run — 설정대로 서버를 띄우고 종료 신호가 올 때까지 연결을 처리한다.
 *   입력: cfg   결과: 정상 종료 0, 시작 실패 1
 */
int server_run(const server_config_t *cfg);

/* server_stopping — 종료 신호(Ctrl+C 등)를 받았으면 1 (모든 스레드에서 확인용) */
int server_stopping(void);

#endif
