#include "thread_pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    pthread_t thread;
    thread_pool *pool;
    int active_fd;
} worker;

struct thread_pool {
    pthread_mutex_t mutex;
    pthread_cond_t available;
    worker *workers;
    size_t started;
    client_job *queue;
    size_t capacity;
    size_t head;
    size_t count;
    bool stopping;
    client_handler handler;
    void *context;
};

static void *run_worker(void *argument)
{
    worker *self = argument;
    thread_pool *pool = self->pool;

    for (;;) {
        client_job job;

        pthread_mutex_lock(&pool->mutex);
        while (pool->count == 0 && !pool->stopping) {
            pthread_cond_wait(&pool->available, &pool->mutex);
        }
        if (pool->stopping) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        job = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        --pool->count;
        self->active_fd = job.client_fd;
        pthread_mutex_unlock(&pool->mutex);

        /* 요청 수신·파일 전송 중에는 큐의 mutex를 잡지 않는다. */
        pool->handler(&job, pool->context);

        pthread_mutex_lock(&pool->mutex);
        /* 종료 담당의 shutdown과 close/FD 재사용이 겹치지 않게 한다. */
        self->active_fd = -1;
        close(job.client_fd);
        pthread_mutex_unlock(&pool->mutex);
    }
    return NULL;
}

thread_pool *thread_pool_create(size_t workers, size_t capacity,
                                client_handler handler, void *context)
{
    thread_pool *pool;
    int error;

    if (workers == 0 || capacity == 0 || handler == NULL) {
        errno = EINVAL;
        return NULL;
    }
    pool = calloc(1, sizeof(*pool));
    if (pool == NULL) return NULL;
    pool->workers = calloc(workers, sizeof(*pool->workers));
    pool->queue = calloc(capacity, sizeof(*pool->queue));
    if (pool->workers == NULL || pool->queue == NULL) {
        error = ENOMEM;
        goto free_storage;
    }
    pool->capacity = capacity;
    pool->handler = handler;
    pool->context = context;
    for (size_t i = 0; i < workers; ++i) {
        pool->workers[i].pool = pool;
        pool->workers[i].active_fd = -1;
    }
    error = pthread_mutex_init(&pool->mutex, NULL);
    if (error != 0) goto free_storage;
    error = pthread_cond_init(&pool->available, NULL);
    if (error != 0) {
        pthread_mutex_destroy(&pool->mutex);
        goto free_storage;
    }
    for (size_t i = 0; i < workers; ++i) {
        error = pthread_create(&pool->workers[i].thread, NULL,
                               run_worker, &pool->workers[i]);
        if (error != 0) {
            thread_pool_destroy(pool);
            errno = error;
            return NULL;
        }
        ++pool->started;
    }
    return pool;

free_storage:
    free(pool->queue);
    free(pool->workers);
    free(pool);
    errno = error;
    return NULL;
}

int thread_pool_submit(thread_pool *pool, const client_job *job)
{
    int result = THREAD_POOL_ACCEPTED;

    pthread_mutex_lock(&pool->mutex);
    if (pool->stopping) {
        result = THREAD_POOL_STOPPED;
    } else if (pool->count == pool->capacity) {
        result = THREAD_POOL_FULL;
    } else {
        size_t tail = (pool->head + pool->count) % pool->capacity;

        /* accept 루프의 지역 변수를 참조하지 않고 연결 정보를 값으로 저장한다. */
        pool->queue[tail] = *job;
        ++pool->count;
        pthread_cond_signal(&pool->available);
    }
    pthread_mutex_unlock(&pool->mutex);
    return result;
}

bool thread_pool_is_stopping(thread_pool *pool)
{
    bool stopping;

    pthread_mutex_lock(&pool->mutex);
    stopping = pool->stopping;
    pthread_mutex_unlock(&pool->mutex);
    return stopping;
}

void thread_pool_destroy(thread_pool *pool)
{
    if (pool == NULL) return;
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    while (pool->count != 0) {
        close(pool->queue[pool->head].client_fd);
        pool->head = (pool->head + 1) % pool->capacity;
        --pool->count;
    }
    for (size_t i = 0; i < pool->started; ++i) {
        if (pool->workers[i].active_fd >= 0) {
            /* close는 워커가 맡는다. 여기서 닫으면 다른 파일의 FD로 재사용될 수 있다. */
            shutdown(pool->workers[i].active_fd, SHUT_RDWR);
        }
    }
    pthread_cond_broadcast(&pool->available);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->started; ++i) {
        pthread_join(pool->workers[i].thread, NULL);
    }
    pthread_cond_destroy(&pool->available);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->queue);
    free(pool->workers);
    free(pool);
}
