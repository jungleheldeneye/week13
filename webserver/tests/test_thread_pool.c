#include "thread_pool.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { JOBS = 48 };

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    thread_pool *pool;
    unsigned started;
    unsigned finished;
    unsigned interrupted;
    unsigned active;
    unsigned maximum_active;
    unsigned seen[JOBS];
    unsigned order[JOBS];
} observations;

static void initialize(observations *state)
{
    *state = (observations){0};
    CHECK(pthread_mutex_init(&state->mutex, NULL) == 0);
    CHECK(pthread_cond_init(&state->changed, NULL) == 0);
}

static void finish(observations *state)
{
    CHECK(pthread_cond_destroy(&state->changed) == 0);
    CHECK(pthread_mutex_destroy(&state->mutex) == 0);
}

static void handle_job(const client_job *job, void *argument)
{
    observations *state = argument;
    unsigned id = ntohs(job->peer.sin_port);
    unsigned char *byte = malloc(1);
    ssize_t received;

    CHECK(byte != NULL && id < JOBS);
    CHECK(pthread_mutex_lock(&state->mutex) == 0);
    CHECK(state->started < JOBS);
    state->order[state->started++] = id;
    ++state->seen[id];
    ++state->active;
    if (state->active > state->maximum_active) state->maximum_active = state->active;
    CHECK(pthread_cond_broadcast(&state->changed) == 0);
    CHECK(pthread_mutex_unlock(&state->mutex) == 0);

    /* 메인이 데이터를 줄 때까지 막힌다. 다른 워커는 큐에서 작업을 꺼낼 수 있어야 한다. */
    do {
        received = recv(job->client_fd, byte, 1, 0);
    } while (received == -1 && errno == EINTR);
    if (received == 1) {
        *byte = (unsigned char)id;
        CHECK(send(job->client_fd, byte, 1, 0) == 1);
    } else {
        CHECK(thread_pool_is_stopping(state->pool));
    }
    free(byte);

    CHECK(pthread_mutex_lock(&state->mutex) == 0);
    --state->active;
    ++state->finished;
    if (received != 1) ++state->interrupted;
    CHECK(pthread_cond_broadcast(&state->changed) == 0);
    CHECK(pthread_mutex_unlock(&state->mutex) == 0);
}

static void wait_started(observations *state, unsigned count)
{
    struct timespec deadline;

    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    CHECK(pthread_mutex_lock(&state->mutex) == 0);
    while (state->started < count) {
        CHECK(pthread_cond_timedwait(&state->changed, &state->mutex, &deadline) == 0);
    }
    CHECK(pthread_mutex_unlock(&state->mutex) == 0);
}

static client_job make_job(int sockets[2], unsigned id)
{
    struct timeval timeout = {.tv_sec = 3};

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    return (client_job){.client_fd = sockets[0], .peer = {.sin_port = htons((uint16_t)id)}};
}

static void complete(int peer, unsigned id)
{
    unsigned char byte = 0;

    CHECK(send(peer, &byte, 1, 0) == 1);
    CHECK(recv(peer, &byte, 1, 0) == 1 && byte == id);
    CHECK(recv(peer, &byte, 1, 0) == 0);
    close(peer);
}

static void test_fifo_capacity_and_copy(void)
{
    observations state;
    int sockets[4][2];
    client_job job;

    initialize(&state);
    state.pool = thread_pool_create(1, 2, handle_job, &state);
    CHECK(state.pool != NULL);
    job = make_job(sockets[0], 0);
    CHECK(thread_pool_submit(state.pool, &job) == THREAD_POOL_ACCEPTED);
    wait_started(&state, 1);
    for (unsigned id = 1; id < 3; ++id) {
        job = make_job(sockets[id], id);
        CHECK(thread_pool_submit(state.pool, &job) == THREAD_POOL_ACCEPTED);
    }
    job = make_job(sockets[3], 3);
    CHECK(thread_pool_submit(state.pool, &job) == THREAD_POOL_FULL);
    CHECK(fcntl(job.client_fd, F_GETFD) != -1); /* 거절된 연결은 호출자가 소유한다. */
    close(sockets[3][0]);
    close(sockets[3][1]);
    job = (client_job){.client_fd = -1}; /* 큐가 지역 변수의 복사본을 가지고 있어야 한다. */
    for (unsigned id = 0; id < 3; ++id) complete(sockets[id][1], id);
    thread_pool_destroy(state.pool);
    CHECK(state.started == 3 && state.finished == 3 && state.maximum_active == 1);
    for (unsigned id = 0; id < 3; ++id) CHECK(state.order[id] == id && state.seen[id] == 1);
    finish(&state);
}

static void test_parallel_exactly_once_and_reuse(void)
{
    observations state;
    int sockets[JOBS][2];

    initialize(&state);
    state.pool = thread_pool_create(4, JOBS, handle_job, &state);
    CHECK(state.pool != NULL);
    for (unsigned id = 0; id < JOBS; ++id) {
        client_job job = make_job(sockets[id], id);

        CHECK(thread_pool_submit(state.pool, &job) == THREAD_POOL_ACCEPTED);
    }
    wait_started(&state, 4);
    /* 4명이 동시에 recv 중이어도 큐를 꺼내는 mutex가 막혀 있지 않다. */
    for (unsigned id = 0; id < JOBS; ++id) {
        unsigned char byte = 0;

        CHECK(send(sockets[id][1], &byte, 1, 0) == 1);
    }
    for (unsigned id = 0; id < JOBS; ++id) {
        unsigned char byte;

        CHECK(recv(sockets[id][1], &byte, 1, 0) == 1 && byte == id);
        CHECK(recv(sockets[id][1], &byte, 1, 0) == 0);
        close(sockets[id][1]);
    }
    thread_pool_destroy(state.pool);
    CHECK(state.started == JOBS && state.finished == JOBS && state.maximum_active == 4);
    for (unsigned id = 0; id < JOBS; ++id) CHECK(state.seen[id] == 1);
    finish(&state);
}

static void test_immediate_shutdown(void)
{
    observations state;
    int sockets[4][2];

    initialize(&state);
    state.pool = thread_pool_create(2, 2, handle_job, &state);
    CHECK(state.pool != NULL);
    for (unsigned id = 0; id < 2; ++id) {
        client_job job = make_job(sockets[id], id);

        CHECK(thread_pool_submit(state.pool, &job) == THREAD_POOL_ACCEPTED);
    }
    wait_started(&state, 2);
    for (unsigned id = 2; id < 4; ++id) {
        client_job job = make_job(sockets[id], id);

        CHECK(thread_pool_submit(state.pool, &job) == THREAD_POOL_ACCEPTED);
    }
    thread_pool_destroy(state.pool); /* recv 중인 2명은 깨우고 큐의 2건은 실행하지 않는다. */
    CHECK(state.started == 2 && state.finished == 2 && state.interrupted == 2);
    for (unsigned id = 0; id < 4; ++id) {
        unsigned char byte;

        CHECK(recv(sockets[id][1], &byte, 1, 0) == 0);
        CHECK(fcntl(sockets[id][0], F_GETFD) == -1 && errno == EBADF);
        close(sockets[id][1]);
    }
    CHECK(state.seen[2] == 0 && state.seen[3] == 0);
    finish(&state);
}

int main(void)
{
    observations state;

    CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    CHECK(thread_pool_create(0, 1, handle_job, NULL) == NULL && errno == EINVAL);
    CHECK(thread_pool_create(1, 0, handle_job, NULL) == NULL && errno == EINVAL);
    CHECK(thread_pool_create(1, 1, NULL, NULL) == NULL && errno == EINVAL);
    initialize(&state);
    state.pool = thread_pool_create(2, 1, handle_job, &state);
    CHECK(state.pool != NULL);
    thread_pool_destroy(state.pool); /* 빈 큐에서 잠든 워커도 종료한다. */
    finish(&state);
    test_fifo_capacity_and_copy();
    test_parallel_exactly_once_and_reuse();
    test_immediate_shutdown();
    puts("PASS: pool FIFO, queue capacity, job copies, socket ownership, worker reuse, "
         "parallel exactly-once dispatch, queued/active/idle shutdown");
    return EXIT_SUCCESS;
}
