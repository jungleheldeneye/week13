/*
 * server.c — 서버 시작/종료와 동시 처리 방식 선택
 *
 * 이 파일이 하는 일:
 *   1) 시그널 설정 (SIGPIPE 무시, SIGINT/SIGTERM → 종료 플래그)
 *   2) net_listen으로 listening 소켓 준비
 *   3) -m 옵션에 따라 연결을 처리하는 방식을 고른다
 *        pool   : pool.c (스레드 풀 + poll 대기실) — 기본
 *        thread : 연결마다 새 스레드가 conn_run_blocking
 *        fork   : 연결마다 자식 프로세스가 conn_run_blocking
 *   요청 하나를 처리하는 실제 로직은 세 방식 모두 conn.c의 conn_serve_one 하나를 공유한다.
 */
#include "server.h"
#include "net.h"
#include "conn.h"
#include "pool.h"
#include "response.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 종료 플래그: 시그널 처리기가 쓰고 여러 스레드가 읽는다.
 * volatile sig_atomic_t는 "시그널 처리기 ↔ 같은 스레드"까지만 안전해서,
 * 스레드 사이에서도 안전한 C11 atomic을 쓴다 (lock-free라 시그널 처리기에서도 사용 가능) */
static atomic_int g_stop = 0;

int server_stopping(void) { return atomic_load(&g_stop); }

/* on_stop — SIGINT/SIGTERM 처리기: 종료 플래그만 세운다 (처리기 안에선 최소한의 일만) */
static void on_stop(int sig)
{
    (void)sig;
    int saved = errno;           /* 처리기가 끼어든 지점의 errno를 망가뜨리지 않게 보존 */
    atomic_store(&g_stop, 1);
    errno = saved;
}

/*
 * setup_signals — 서버에 필요한 시그널 동작을 설정한다.
 *   SIGPIPE: 끊긴 소켓에 write하면 프로세스가 죽는 기본 동작을 끄고, write가 -1(EPIPE)을
 *            돌려주게 한다 → 클라이언트 하나가 끊겨도 서버 전체가 죽지 않음.
 *   SIGINT/SIGTERM: SA_RESTART를 일부러 빼서, accept()/poll()에서 기다리던 중이면
 *            EINTR로 깨어나 루프를 빠져나오게 한다.
 */
static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* accept_one — 연결 하나를 받는다. 결과: 연결 객체, 없거나 실패면 NULL (종료 신호 포함) */
static conn_t *accept_one(int lfd)
{
    struct sockaddr_storage ss;           /* IPv4/IPv6 어느 쪽 주소든 담을 수 있는 크기 */
    socklen_t sl = sizeof(ss);
    int cfd = accept(lfd, (struct sockaddr *)&ss, &sl);
    if (cfd < 0) {
        if (errno == EMFILE || errno == ENFILE) {      /* fd 고갈: 잠깐 쉬어 바쁜 반복 방지 */
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        } else if (errno != EINTR && errno != ECONNABORTED) {
            perror("accept");
        }
        return NULL;
    }
    conn_t *c = conn_new(cfd, (struct sockaddr *)&ss, sl);
    if (!c) close(cfd);
    return c;
}

/* ---------- -m thread: 연결마다 스레드 ---------- */

#define THREAD_MODE_MAX 1024              /* 동시에 살아 있는 스레드 상한: 넘으면 503 */
static atomic_int g_active = 0;           /* 여러 스레드가 동시에 바꾸므로 atomic */
static const server_config_t *g_cfg;

static void *thread_main(void *arg)
{
    static atomic_int seq = 0;
    char who[16];
    snprintf(who, sizeof(who), "t%d", atomic_fetch_add(&seq, 1));
    conn_run_blocking((conn_t *)arg, g_cfg, who);
    atomic_fetch_sub(&g_active, 1);
    return NULL;
}

static void run_thread_mode(int lfd)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);   /* 끝나면 알아서 정리 (join 불필요) */

    /* 새 스레드는 Ctrl+C를 받지 않도록: 신호는 accept 중인 메인이 받아야 루프를 빠져나옴 */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);

    while (!server_stopping()) {
        conn_t *c = accept_one(lfd);
        if (!c) continue;
        if (atomic_load(&g_active) >= THREAD_MODE_MAX) {
            resp_error(c->fd, 503, 0, 0, "Retry-After: 1\r\n");
            conn_free(c);
            continue;
        }
        atomic_fetch_add(&g_active, 1);
        pthread_t t;
        pthread_sigmask(SIG_BLOCK, &block, &old);
        int rc = pthread_create(&t, &attr, thread_main, c);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        if (rc != 0) {
            atomic_fetch_sub(&g_active, 1);
            resp_error(c->fd, 503, 0, 0, NULL);
            conn_free(c);
        }
    }
    pthread_attr_destroy(&attr);

    /* 처리 중인 연결이 끝나길 최대 15초 기다린다 (각 연결은 시간 제한이 있어 결국 끝남) */
    for (int i = 0; i < 150 && atomic_load(&g_active) > 0; i++) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

/* ---------- -m fork: 연결마다 자식 프로세스 ---------- */

static void run_fork_mode(int lfd)
{
    /* 자식이 끝나면 자동으로 정리되게 (좀비 프로세스 방지) */
    signal(SIGCHLD, SIG_IGN);

    while (!server_stopping()) {
        conn_t *c = accept_one(lfd);
        if (!c) continue;
        pid_t pid = fork();
        if (pid == 0) {
            /* 자식: listening 소켓은 필요 없으니 닫고, 연결 하나만 처리하고 끝낸다 */
            close(lfd);
            signal(SIGCHLD, SIG_DFL);     /* 7단계 CGI에서 자식의 자식을 waitpid로 기다릴 수 있게 */
            char who[24];
            snprintf(who, sizeof(who), "pid %d", (int)getpid());
            conn_run_blocking(c, g_cfg, who);
            fflush(stdout);
            _exit(0);                     /* exit 대신 _exit: 부모의 정리 작업을 자식이 중복 실행하지 않게 */
        }
        if (pid < 0) {
            perror("fork");
            resp_error(c->fd, 503, 0, 0, NULL);
        }
        /* 부모: 연결은 자식이 가져갔으니 내 쪽 fd만 닫는다 (커널 참조 수 감소, 연결은 유지됨) */
        conn_free(c);
    }
}

int server_run(const server_config_t *cfg)
{
    char err[256];

    setup_signals();
    g_cfg = cfg;

    int lfd = net_listen(cfg->bind_addr, cfg->port, SERVER_BACKLOG, err, sizeof(err));
    if (lfd < 0) {
        fprintf(stderr, "오류: %s\n", err);
        return 1;
    }
    printf("서버 시작: http://%s:%d  (종료: Ctrl+C)\n",
           strcmp(cfg->bind_addr, "0.0.0.0") == 0 ? "localhost" : cfg->bind_addr, cfg->port);

    int rc = 0;
    switch (cfg->mode) {
    case MODE_POOL:   rc = pool_run(lfd, cfg) == 0 ? 0 : 1; break;
    case MODE_THREAD: run_thread_mode(lfd); break;
    case MODE_FORK:   run_fork_mode(lfd); break;
    }

    close(lfd);
    printf("\n서버 종료\n");
    return rc;
}
