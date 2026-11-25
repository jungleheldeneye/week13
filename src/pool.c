/*
 * pool.c — 스레드 풀 + poll 대기실 구현 (pool.h 설명 참고)
 *
 * 스레드 사이에서 공유하는 것과 보호 방법:
 *   - 작업 대기열(g_jobs): 문지기가 넣고 워커가 꺼냄 → mutex + 조건 변수
 *   - 반납함(g_ret):      워커가 keep-alive 연결을 넣고 문지기가 꺼냄 → mutex
 *                         + "깨우기 파이프"에 1바이트 써서 poll 중인 문지기를 깨움
 *   - 대기실(room):        문지기만 만짐 → 잠금 불필요
 */
#include "pool.h"
#include "conn.h"
#include "server.h"
#include "response.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 작업 대기열: 고정 크기 원형 버퍼 */
static struct {
    conn_t         *items[POOL_QUEUE_MAX];
    int             head, count;
    int             closing;          /* 1이면 워커들이 끝내고 나감 */
    pthread_mutex_t mu;
    pthread_cond_t  cv;               /* "일이 생겼다" 알림 */
} g_jobs = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/* 반납함: 워커가 처리를 마친 keep-alive 연결 */
static struct {
    conn_t         *items[POOL_MAX_CONNS];
    int             count;
    pthread_mutex_t mu;
} g_ret = { .mu = PTHREAD_MUTEX_INITIALIZER };

static int g_wake[2] = {-1, -1};      /* 깨우기 파이프: [0] 읽기(문지기 poll), [1] 쓰기(워커) */
static const server_config_t *g_cfg;

/* reject_503 — 자리가 없을 때 정중히 거절하고 닫는다 */
static void reject_503(conn_t *c, const char *why)
{
    printf("[문지기] %s 503 (%s)\n", c->peer, why);
    resp_error(c->fd, 503, 0, 0, "Retry-After: 1\r\n");
    conn_free(c);
}

/* jobs_push — 작업 대기열에 연결을 넣는다. 결과: 0 성공, -1 꽉 참 */
static int jobs_push(conn_t *c)
{
    pthread_mutex_lock(&g_jobs.mu);
    if (g_jobs.count == POOL_QUEUE_MAX) {
        pthread_mutex_unlock(&g_jobs.mu);
        return -1;
    }
    g_jobs.items[(g_jobs.head + g_jobs.count) % POOL_QUEUE_MAX] = c;
    g_jobs.count++;
    pthread_cond_signal(&g_jobs.cv);       /* 자고 있는 워커 하나를 깨운다 */
    pthread_mutex_unlock(&g_jobs.mu);
    return 0;
}

/* jobs_pop — 일이 생길 때까지 기다렸다가 하나 꺼낸다. 종료 중이면 NULL */
static conn_t *jobs_pop(void)
{
    pthread_mutex_lock(&g_jobs.mu);
    /* while인 이유: 깨어났는데 다른 워커가 먼저 가져갔을 수 있음(가짜 깨어남 포함) */
    while (g_jobs.count == 0 && !g_jobs.closing)
        pthread_cond_wait(&g_jobs.cv, &g_jobs.mu);
    conn_t *c = NULL;
    if (!g_jobs.closing) {
        c = g_jobs.items[g_jobs.head];
        g_jobs.head = (g_jobs.head + 1) % POOL_QUEUE_MAX;
        g_jobs.count--;
    }
    pthread_mutex_unlock(&g_jobs.mu);
    return c;
}

/* give_back — keep-alive 연결을 반납함에 넣고 문지기를 깨운다 */
static void give_back(conn_t *c)
{
    pthread_mutex_lock(&g_ret.mu);
    if (g_ret.count == POOL_MAX_CONNS) {
        pthread_mutex_unlock(&g_ret.mu);
        conn_free(c);
        return;
    }
    g_ret.items[g_ret.count++] = c;
    pthread_mutex_unlock(&g_ret.mu);
    ssize_t w = write(g_wake[1], "x", 1);  /* 파이프가 꽉 차도(EAGAIN) 이미 깨울 신호가 있으니 무시 */
    (void)w;
}

/* worker_main — 워커 스레드: 대기열에서 연결을 꺼내 처리하는 일을 반복 */
static void *worker_main(void *arg)
{
    char who[16];
    snprintf(who, sizeof(who), "w%d", (int)(long)arg);

    conn_t *c;
    while ((c = jobs_pop()) != NULL) {
        int keep;
        /* 다음 요청이 이미 버퍼에 와 있으면(파이프라이닝) poll이 알려 주지 않으므로 여기서 바로 처리 */
        do {
            keep = conn_serve_one(c, g_cfg, who);
        } while (keep && net_reader_pending(&c->r) > 0);

        if (keep) give_back(c);           /* 다음 요청은 대기실에서 기다리게 */
        else conn_free(c);
    }
    return NULL;
}

int pool_run(int lfd, const server_config_t *cfg)
{
    g_cfg = cfg;
    if (pipe(g_wake) != 0) { perror("pipe"); return -1; }
    for (int i = 0; i < 2; i++) {
        net_set_nonblock(g_wake[i], 1);
        fcntl(g_wake[i], F_SETFD, FD_CLOEXEC);   /* 7단계 CGI 자식 프로세스에 새어 나가지 않게 */
    }
    net_set_nonblock(lfd, 1);    /* accept를 "있는 만큼만" 하고 바로 돌아오게 */

    /* 워커는 Ctrl+C를 받지 않게 막아 둔다 → 신호는 문지기(메인)가 받아 poll에서 깨어남 */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);

    pthread_t *tids = calloc((size_t)cfg->threads, sizeof(*tids));
    int started = 0;
    for (; tids && started < cfg->threads; started++)
        if (pthread_create(&tids[started], NULL, worker_main, (void *)(long)started) != 0) break;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (started == 0) { fprintf(stderr, "오류: 워커 스레드를 만들 수 없습니다\n"); free(tids); return -1; }

    static conn_t *room[POOL_MAX_CONNS];          /* 대기실 */
    static struct pollfd pfd[POOL_MAX_CONNS + 2];
    int nroom = 0;
    time_t accept_paused_until = 0;

    while (!server_stopping()) {
        time_t now = time(NULL);
        int can_accept = nroom < POOL_MAX_CONNS && now >= accept_paused_until;

        pfd[0].fd = can_accept ? lfd : -1;        /* fd가 음수면 poll이 무시 */
        pfd[0].events = POLLIN;
        pfd[1].fd = g_wake[0];
        pfd[1].events = POLLIN;
        for (int i = 0; i < nroom; i++) {
            pfd[2 + i].fd = room[i]->fd;
            pfd[2 + i].events = POLLIN;           /* "읽을 데이터가 왔거나 상대가 끊음" */
        }

        /* 최대 1초만 기다림: 아무 일이 없어도 깨어나 시간 초과와 종료 신호를 확인 */
        int n = poll(pfd, (nfds_t)(2 + nroom), 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        now = time(NULL);

        /* 1) 대기실: 데이터가 온 연결은 워커에게, 오래 조용한 연결은 닫기.
         *    뒤에서부터 돌며 빠진 자리를 마지막 원소로 메운다 (이미 확인한 원소만 옮겨지므로 안전) */
        for (int i = nroom - 1; i >= 0; i--) {
            conn_t *c = room[i];
            int ready = pfd[2 + i].revents & (POLLIN | POLLHUP | POLLERR);
            int limit = c->served == 0 ? SERVER_TIMEOUT_S : SERVER_KEEPALIVE_S;
            if (!ready && now - c->last < limit) continue;

            room[i] = room[--nroom];
            if (!ready) conn_free(c);                         /* 시간 초과: 조용히 닫기 */
            else if (jobs_push(c) != 0) reject_503(c, "작업 대기열 가득 참");
        }

        /* 2) 반납함: 워커가 돌려준 keep-alive 연결을 대기실로 */
        if (pfd[1].revents & POLLIN) {
            char tmp[256];
            while (read(g_wake[0], tmp, sizeof(tmp)) > 0)      /* 깨우기 신호 비우기 */
                ;
            pthread_mutex_lock(&g_ret.mu);
            for (int i = 0; i < g_ret.count; i++) {
                if (nroom < POOL_MAX_CONNS) room[nroom++] = g_ret.items[i];
                else conn_free(g_ret.items[i]);
            }
            g_ret.count = 0;
            pthread_mutex_unlock(&g_ret.mu);
        }

        /* 3) 새 연결: 대기열에 쌓인 만큼 accept해서 대기실에 넣기 (워커는 아직 배정 안 함) */
        if (pfd[0].fd >= 0 && (pfd[0].revents & POLLIN)) {
            for (;;) {
                struct sockaddr_storage ss;
                socklen_t sl = sizeof(ss);
                int cfd = accept(lfd, (struct sockaddr *)&ss, &sl);
                if (cfd < 0) {
                    if (errno == EMFILE || errno == ENFILE) {   /* fd 고갈: 1초 쉬었다가 다시 */
                        fprintf(stderr, "[문지기] fd 부족: 잠시 접속을 받지 않습니다\n");
                        accept_paused_until = now + 1;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR
                               && errno != ECONNABORTED) {
                        perror("accept");
                    }
                    break;                                       /* EAGAIN = 이번에 올 연결은 다 받음 */
                }
                net_set_nonblock(cfd, 0);   /* Mac은 listening 소켓의 non-blocking을 물려받으므로 되돌림 */
                conn_t *c = conn_new(cfd, (struct sockaddr *)&ss, sl);
                if (!c) { close(cfd); continue; }
                if (nroom >= POOL_MAX_CONNS) { reject_503(c, "대기실 가득 참"); continue; }
                room[nroom++] = c;
            }
        }
    }

    /* 종료: 워커를 깨워 내보내고, 남은 연결을 모두 닫는다 */
    pthread_mutex_lock(&g_jobs.mu);
    g_jobs.closing = 1;
    pthread_cond_broadcast(&g_jobs.cv);
    pthread_mutex_unlock(&g_jobs.mu);
    for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
    free(tids);

    for (int i = 0; i < g_jobs.count; i++)
        conn_free(g_jobs.items[(g_jobs.head + i) % POOL_QUEUE_MAX]);
    for (int i = 0; i < g_ret.count; i++) conn_free(g_ret.items[i]);
    for (int i = 0; i < nroom; i++) conn_free(room[i]);
    close(g_wake[0]);
    close(g_wake[1]);
    return 0;
}
