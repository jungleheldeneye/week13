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
/* 워커가 연결하는 백엔드도 종료 시 깨울 수 있도록 등록한다. 0은 성공, -1은 종료/잘못된 연결. */
int thread_pool_attach_backend(thread_pool *pool, int client_fd, int backend_fd);
/* 등록 해제와 close를 같은 mutex 안에서 수행해 FD 재사용과 shutdown의 경합을 막는다. */
void thread_pool_close_backend(thread_pool *pool, int client_fd, int backend_fd);
/* 큐를 폐기하고 활성 소켓을 shutdown한 뒤 워커를 회수한다.
 * submit과 destroy는 접속 담당 스레드가 호출한다. 시그널 핸들러에서 호출하지 않는다.
 */
void thread_pool_destroy(thread_pool *pool);

#endif
