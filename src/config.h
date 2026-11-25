/*
 * config.h — 서버 실행 설정
 *
 * 이 파일이 하는 일:
 *   명령줄 인자(-p 포트, -r 정적 파일 루트, -b 바인드 주소, -c 라우팅 설정 파일,
 *   -m 동시 처리 방식, -t 워커 수, -f Forward Proxy 허용)를
 *   읽어서 server_config_t 구조체 하나에 모아 둔다. 서버의 다른 모듈은
 *   argv를 직접 보지 않고 이 구조체만 본다.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>

#define CONFIG_DEFAULT_BIND "0.0.0.0"   /* -b 생략 시: 모든 네트워크 주소에서 접속 허용 */
#define CONFIG_MAX_THREADS  1024

/* 동시 처리 방식 (-m) */
typedef enum {
    MODE_POOL,     /* 스레드 풀 + poll 대기실 (기본) */
    MODE_THREAD,   /* 연결마다 스레드 */
    MODE_FORK      /* 연결마다 프로세스 */
} server_mode_t;

typedef struct {
    int  port;                  /* 서버가 bind할 포트 (1~65535) */
    char bind_addr[256];        /* bind할 주소. 예: 0.0.0.0(전체), 127.0.0.1(내 컴퓨터만) */
    char docroot[PATH_MAX];     /* 정적 파일 루트의 절대 경로 (경로 순회 검사의 기준) */
    char routes_path[PATH_MAX]; /* 라우팅 규칙 파일 경로 (없으면 빈 문자열) */
    server_mode_t mode;         /* -m pool|thread|fork */
    int  threads;               /* -t: 스레드 풀 워커 수 (기본: CPU 코어 수 x 4, 4~64) */
    int  forward;               /* -f: Forward Proxy 허용 (기본 꺼짐: 아무나 경유지로 쓰는 오픈 프록시 방지) */
} server_config_t;

/*
 * config_parse_args — argv를 읽어 cfg를 채운다.
 *   입력: argc/argv(main 그대로), cfg(채울 곳), err/errlen(실패 이유를 적을 버퍼)
 *   결과: 성공 0, 실패 -1 (err에 사람이 읽을 이유가 들어감)
 */
int config_parse_args(int argc, char *argv[], server_config_t *cfg,
                      char *err, int errlen);

/* config_print_usage — 사용법 안내를 stderr에 출력한다. 입력: 프로그램 이름 */
void config_print_usage(const char *prog);

/* config_mode_name — 방식 이름 문자열 ("pool" 등, 로그용) */
const char *config_mode_name(server_mode_t m);

#endif
