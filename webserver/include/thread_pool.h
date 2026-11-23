#ifndef WEBSERVER_THREAD_POOL_H
#define WEBSERVER_THREAD_POOL_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>

/* 부하 테스트 전 초기값. 실행 중 워커 수는 바뀌지 않는다. */
#define THREAD_POOL_WORKERS 4
#define THREAD_POOL_QUEUE_CAPACITY 64

typedef struct thread_pool thread_pool;

typedef struct {
    int client_fd;
    struct sockaddr_in peer;
} client_job;

/* 콜백은 파일·요청 메모리를 정리한다. client_fd는 풀이 닫는다. */
typedef void (*client_handler)(const client_job *job, void *context);

enum {
    THREAD_POOL_ACCEPTED = 0,
    THREAD_POOL_FULL = 1,
    THREAD_POOL_STOPPED = 2
};

thread_pool *thread_pool_create(size_t workers, size_t capacity,
                                client_handler handler, void *context);
/* 대기하지 않는다. ACCEPTED일 때만 소켓 소유권이 풀로 넘어간다. */
int thread_pool_submit(thread_pool *pool, const client_job *job);
bool thread_pool_is_stopping(thread_pool *pool);
/* 큐를 폐기하고 활성 소켓을 shutdown한 뒤 워커를 회수한다.
 * submit과 destroy는 접속 담당 스레드가 호출한다. 시그널 핸들러에서 호출하지 않는다.
 */
void thread_pool_destroy(thread_pool *pool);

#endif
