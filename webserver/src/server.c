#include "server.h"
#include "http.h"
#include "router.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    LISTEN_BACKLOG = 128,
    FIRST_BYTE_CAPACITY = 128,
    FIRST_BYTE_TIMEOUT_MS = 30000,
    LISTENER_EVENT = 0,
    WAKE_EVENT = 1,
    FIRST_CLIENT_EVENT = 2
};

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t wake_write_fd = -1;

typedef struct {
    router routes;
    thread_pool *pool;
    reverse_proxy proxy;
} server_context;

typedef struct {
    client_job job;
    int64_t expires_at;
} waiting_client;

void server_request_stop(void)
{
    int saved_errno = errno;
    int fd = wake_write_fd;

    stop_requested = 1;
    if (fd >= 0) {
        unsigned char byte = 1;
        ssize_t result;

        /* 핸들러는 mutex를 만지지 않고 논블로킹 파이프로 메인만 깨운다. */
        do {
            result = write(fd, &byte, sizeof(byte));
        } while (result == -1 && errno == EINTR);
    }
    errno = saved_errno;
}

static int prepare_fd(int fd, int nonblocking)
{
    int flags = fcntl(fd, F_GETFL);
    int descriptor_flags = fcntl(fd, F_GETFD);

    if (flags == -1 || descriptor_flags == -1) return -1;
    if (fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == -1) return -1;
    return fcntl(fd, F_SETFL, nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}

static int create_listener(uint16_t port, uint16_t *bound_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse_address = 1;
    struct sockaddr_in address = {0};

    if (fd == -1) {
        return -1;
    }
    if (prepare_fd(fd, 1) == -1) goto failed;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1) {
        goto failed;
    }

    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        errno = EINVAL;
        goto failed;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        goto failed;
    }
    if (listen(fd, LISTEN_BACKLOG) == -1) {
        goto failed;
    }
    {
        socklen_t length = sizeof(address);

        if (getsockname(fd, (struct sockaddr *)&address, &length) == -1) {
            goto failed;
        }
        *bound_port = ntohs(address.sin_port);
    }

    return fd;

failed: {
    int saved_errno = errno;

    close(fd);
    errno = saved_errno;
    return -1;
}
}

static void handle_client(const client_job *job, void *argument)
{
    server_context *context = argument;
    int client_fd = job->client_fd;
    const struct sockaddr_in *peer = &job->peer;
    char ip[INET_ADDRSTRLEN];
    const char *address = inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip));
    http_reader reader;
    http_request request;
    unsigned request_count = 0;
    int status;

    http_reader_init(&reader);
    http_request_init(&request);
    if (thread_pool_is_stopping(context->pool)) {
        goto done;
    }
    printf("클라이언트 연결됨: %s:%u\n",
           address != NULL ? address : "unknown", (unsigned)ntohs(peer->sin_port));
    if (http_set_socket_timeout(client_fd, HTTP_IDLE_TIMEOUT_MS) == -1) {
        if (!thread_pool_is_stopping(context->pool)) {
            perror("소켓 타임아웃 설정");
        }
        goto done;
    }
    for (;;) {
        status = http_read_request(client_fd, &reader, &request);
        if (thread_pool_is_stopping(context->pool)) break;
        if (status != HTTP_OK) break;
        ++request_count;
        if (request_count >= HTTP_MAX_REQUESTS_PER_CONNECTION) request.keep_alive = false;
        printf("HTTP 요청: %s %s %s, 본문 %zu바이트\n",
               request.method, request.target, request.version, request.body_length);
        if (router_handle(&context->routes, client_fd, &request) == -1 &&
            !thread_pool_is_stopping(context->pool)) {
            /* 이미 응답이 시작됐을 수 있으므로 추가 오류 응답은 보내지 않는다. */
            perror("HTTP 응답 처리");
            goto done;
        }
        if (!request.keep_alive || thread_pool_is_stopping(context->pool)) goto done;
        /* 연결별 reader는 유지하고, 요청별 본문/헤더 메모리는 즉시 해제한다. */
        http_request_free(&request);
        status = http_wait_next_request(client_fd, &reader, HTTP_KEEP_ALIVE_TIMEOUT_MS);
        if (status != HTTP_OK || thread_pool_is_stopping(context->pool)) goto done;
    }
    if (thread_pool_is_stopping(context->pool)) goto done;
    if (status > 0) {
        printf("HTTP 요청 거절: %d %s\n", status, http_status_reason(status));
        if (http_send_error(client_fd, status) == -1 &&
            !thread_pool_is_stopping(context->pool)) {
            perror("HTTP 오류 응답 전송");
        }
    } else if (status == HTTP_IO_ERROR) {
        perror("HTTP 요청 수신");
    }

done:
    http_request_free(&request);
}

static thread_pool *create_workers(server_context *context)
{
    sigset_t shutdown_signals;
    sigset_t previous_mask;
    thread_pool *pool;
    int error;
    int saved_errno;

    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    error = pthread_sigmask(SIG_BLOCK, &shutdown_signals, &previous_mask);
    if (error != 0) {
        errno = error;
        return NULL;
    }
    /* 워커는 차단된 마스크를 상속한다. 종료 신호는 메인만 처리한다. */
    pool = thread_pool_create(THREAD_POOL_WORKERS, THREAD_POOL_QUEUE_CAPACITY,
                              handle_client, context);
    saved_errno = errno;
    error = pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    if (error != 0) {
        thread_pool_destroy(pool);
        errno = error;
        return NULL;
    }
    errno = saved_errno;
    return pool;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void dispatch_client(thread_pool *pool, const client_job *job)
{
    int submitted = thread_pool_submit(pool, job);

    if (submitted == THREAD_POOL_ACCEPTED) {
        printf("작업 큐 등록: %u\n", (unsigned)ntohs(job->peer.sin_port));
        return;
    }
    if (submitted == THREAD_POOL_FULL) {
        http_send_unavailable(job->client_fd);
        puts("작업 큐 가득 참: 연결 거절 (503 시도)");
    }
    close(job->client_fd);
}

static size_t process_waiting_clients(thread_pool *pool, struct pollfd *events,
                                      waiting_client *clients, size_t count, int64_t now)
{
    size_t remaining = 0;

    for (size_t i = 0; i < count; ++i) {
        short ready = events[i].revents;
        const client_job *job = &clients[i].job;

        if (ready & (POLLERR | POLLNVAL)) {
            close(job->client_fd);
        } else if (ready & POLLIN) {
            /* POLLIN can also indicate EOF. The worker makes the first recv(). */
            dispatch_client(pool, job);
        } else if ((ready & POLLHUP) || clients[i].expires_at <= now) {
            close(job->client_fd);
        } else {
            /* Keep accept order so the first retained deadline is the earliest. */
            events[remaining] = events[i];
            clients[remaining++] = clients[i];
        }
    }
    return remaining;
}

int server_run(const server_config *config)
{
    server_context context = {.routes = {.root_fd = -1}, .pool = NULL};
    uint16_t port = config->port;
    int listener = -1;
    int wake_pipe[2] = {-1, -1};
    struct pollfd events[FIRST_CLIENT_EVENT + FIRST_BYTE_CAPACITY];
    waiting_client clients[FIRST_BYTE_CAPACITY];
    size_t waiting_count = 0;
    int result = 0;
    int proxy_ready = 0;

    if (stop_requested) return 0;
    if (pipe(wake_pipe) == -1 || prepare_fd(wake_pipe[0], 1) == -1 ||
        prepare_fd(wake_pipe[1], 1) == -1) {
        perror("종료 알림 파이프 준비 실패");
        result = -1;
        goto done;
    }
    wake_write_fd = wake_pipe[1];
    if (router_init(&context.routes, config->document_root) == -1) {
        perror("문서 루트 열기 실패");
        result = -1;
        goto done;
    }
    if (proxy_init(&context.proxy, config) == -1) {
        perror("프록시 초기화 실패");
        result = -1;
        goto done;
    }
    proxy_ready = 1;
    context.routes.proxy = &context.proxy;
    context.routes.proxy_prefix = config->proxy_prefix;
    listener = create_listener(port, &port);
    if (listener == -1) {
        perror("서버 소켓 준비 실패");
        result = -1;
        goto done;
    }
    if (stop_requested) goto done;
    context.pool = create_workers(&context);
    if (context.pool == NULL) {
        perror("스레드 풀 생성 실패");
        result = -1;
        goto done;
    }
    context.routes.workers = context.pool;
    events[LISTENER_EVENT] = (struct pollfd){.fd = listener, .events = POLLIN};
    events[WAKE_EVENT] = (struct pollfd){.fd = wake_pipe[0], .events = POLLIN};

    if (!stop_requested) {
        printf("스레드 풀: 워커 %d개, 작업 큐 %d칸\n",
               THREAD_POOL_WORKERS, THREAD_POOL_QUEUE_CAPACITY);
        printf("첫 요청 대기: 최대 %d개, %dms\n", FIRST_BYTE_CAPACITY, FIRST_BYTE_TIMEOUT_MS);
        printf("서버 시작: 127.0.0.1:%u (연결 대기 중)\n", (unsigned)port);
        printf("문서 루트: %s\n", config->document_root);
        printf("프록시: %s, 백엔드 %zu개, 무응답 제한 %ums (경로 유지)\n",
               config->proxy_prefix, config->backend_count, config->proxy_timeout_ms);
    }
    while (!stop_requested) {
        client_job job = {0};
        socklen_t peer_length = sizeof(job.peer);
        int timeout = -1;
        int64_t now;

        if (waiting_count != 0) {
            now = monotonic_ms();
            if (now == -1) goto clock_failed;
            if (clients[0].expires_at <= now) timeout = 0;
            else timeout = (int)(clients[0].expires_at - now);
        }
        if (poll(events, FIRST_CLIENT_EVENT + waiting_count, timeout) == -1) {
            if (errno == EINTR) continue;
            perror("접속 대기");
            result = -1;
            break;
        }
        if (stop_requested || events[WAKE_EVENT].revents != 0) break;
        if (events[LISTENER_EVENT].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fputs("대기 소켓 오류\n", stderr);
            result = -1;
            break;
        }
        if (waiting_count != 0) {
            now = monotonic_ms();
            if (now == -1) goto clock_failed;
            waiting_count = process_waiting_clients(context.pool, events + FIRST_CLIENT_EVENT,
                                                    clients, waiting_count, now);
        }
        if (stop_requested || !(events[LISTENER_EVENT].revents & POLLIN)) continue;
        job.client_fd = accept(listener, (struct sockaddr *)&job.peer, &peer_length);
        if (job.client_fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("accept");
            result = -1;
            break;
        }

        if (stop_requested) {
            close(job.client_fd);
            break;
        }
        if (waiting_count == FIRST_BYTE_CAPACITY) {
            close(job.client_fd);
            puts("첫 요청 대기 목록 가득 참: 연결 종료");
            continue;
        }
        /* OS에 따라 accept가 논블로킹 속성을 상속할 수 있어 명시적으로 해제한다. */
        if (prepare_fd(job.client_fd, 0) == -1) {
            perror("클라이언트 소켓 준비");
            close(job.client_fd);
            continue;
        }
        now = monotonic_ms();
        if (now == -1) {
            close(job.client_fd);
            goto clock_failed;
        }
        clients[waiting_count] = (waiting_client){.job = job, .expires_at = now + FIRST_BYTE_TIMEOUT_MS};
        events[FIRST_CLIENT_EVENT + waiting_count] = (struct pollfd){.fd = job.client_fd, .events = POLLIN};
        ++waiting_count;
    }
    goto done;

clock_failed:
    perror("첫 요청 대기 시간 확인 실패");
    result = -1;
done:
    if (listener >= 0) close(listener);
    for (size_t i = 0; i < waiting_count; ++i) close(clients[i].job.client_fd);
    thread_pool_destroy(context.pool);
    if (proxy_ready) proxy_destroy(&context.proxy);
    router_destroy(&context.routes);
    wake_write_fd = -1;
    if (wake_pipe[0] >= 0) close(wake_pipe[0]);
    if (wake_pipe[1] >= 0) close(wake_pipe[1]);
    puts("서버 종료");
    return result;
}
