/*
 * bench.c — 간단한 HTTP 부하 테스트 도구 (ab와 비슷한 사용법)
 *
 * 왜 직접 만들었나:
 *   macOS에는 ab가 기본으로 있지만 Linux 환경에는 없는 경우가 많고, ab는 HTTP/1.0만 써서
 *   chunked 응답(CGI) + keep-alive 조합을 측정하지 못한다. 이 도구는 HTTP/1.1로
 *   Content-Length / chunked / 연결 종료 세 가지 응답 끝 표시를 모두 처리한다.
 *
 * 사용:  ./bench -n 요청수 -c 동시연결수 [-k] http://127.0.0.1:8080/index.html
 *   -k : keep-alive (연결 하나로 여러 요청). 없으면 요청마다 새 연결.
 * 출력:  초당 요청 수, 응답 시간(평균/p50/p90/p99/최대), 실패 수, 상태 코드별 개수
 *
 * 구조: 동시연결수만큼 스레드를 만들고, 각 스레드가 공용 카운터에서 요청을 하나씩 가져가 처리.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static char g_host[256], g_port[16] = "80", g_path[2048] = "/";
static char g_req[4096];
static size_t g_req_len;
static int g_keep;
static long g_total;
static atomic_long g_next;              /* 다음에 보낼 요청 번호 (스레드들이 나눠 가짐) */
static atomic_long g_fail, g_bytes;
static atomic_long g_codes[6];          /* 1xx..5xx 개수 */
static double *g_lat;                   /* 요청별 응답 시간(ms) */
static struct addrinfo *g_ai;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int open_conn(void)
{
    int fd = socket(g_ai->ai_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = {30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, g_ai->ai_addr, g_ai->ai_addrlen) != 0) { close(fd); return -1; }
    return fd;
}

/* 버퍼형 읽기: 응답을 한 번에 여러 개 받을 수 있으므로 남은 데이터를 보관 */
typedef struct { int fd; char buf[65536]; size_t pos, len; } rd_t;

static int rd_fill(rd_t *r)
{
    if (r->pos > 0 && r->pos == r->len) r->pos = r->len = 0;
    if (r->len == sizeof(r->buf)) {     /* 앞쪽 소비분을 당겨서 공간 확보 */
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
        r->len -= r->pos; r->pos = 0;
    }
    ssize_t n;
    do n = read(r->fd, r->buf + r->len, sizeof(r->buf) - r->len); while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;
    r->len += (size_t)n;
    return 0;
}

/* 한 줄 읽기 (CRLF 제거). 결과: 줄 길이, 실패 -1 */
static int rd_line(rd_t *r, char *out, size_t cap)
{
    for (;;) {
        char *nl = memchr(r->buf + r->pos, '\n', r->len - r->pos);
        if (nl) {
            size_t l = (size_t)(nl - (r->buf + r->pos));
            size_t c = l < cap - 1 ? l : cap - 1;
            memcpy(out, r->buf + r->pos, c);
            out[c] = '\0';
            if (c && out[c - 1] == '\r') out[--c] = '\0';
            r->pos += l + 1;
            return (int)c;
        }
        if (rd_fill(r) != 0) return -1;
    }
}

/* n바이트 건너뛰기 (본문은 내용 검사 없이 길이만 셈). n<0이면 연결 끝까지 */
static long rd_skip(rd_t *r, long n)
{
    long got = 0;
    while (n < 0 || got < n) {
        if (r->pos == r->len && rd_fill(r) != 0) return n < 0 ? got : -1;
        long avail = (long)(r->len - r->pos);
        long take = (n < 0 || avail < n - got) ? avail : n - got;
        r->pos += (size_t)take;
        got += take;
    }
    return got;
}

/* 응답 하나를 끝까지 읽는다. 결과: 상태 코드(성공), -1 실패. *close_after = 서버가 연결을 닫는지 */
static int read_response(rd_t *r, int *close_after)
{
    char line[8192];
    if (rd_line(r, line, sizeof(line)) < 0) return -1;
    int code = 0, minor = 1;
    if (sscanf(line, "HTTP/1.%d %d", &minor, &code) != 2) return -1;
    long clen = -1;
    int chunked = 0;
    *close_after = (minor == 0);
    for (;;) {
        int l = rd_line(r, line, sizeof(line));
        if (l < 0) return -1;
        if (l == 0) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) clen = atol(line + 15);
        else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strcasestr(line, "chunked")) chunked = 1;
        else if (strncasecmp(line, "Connection:", 11) == 0) {
            if (strcasestr(line, "close")) *close_after = 1;
            else if (strcasestr(line, "keep-alive")) *close_after = 0;
        }
    }
    long body = 0;
    int no_body = code == 204 || code == 304 || (code >= 100 && code < 200);
    if (no_body) clen = 0;
    if (chunked && !no_body) {
        for (;;) {
            if (rd_line(r, line, sizeof(line)) < 0) return -1;
            long sz = strtol(line, NULL, 16);
            if (sz == 0) {                          /* 마지막 조각 + 트레일러 끝 빈 줄 */
                while ((rd_line(r, line, sizeof(line))) > 0) ;
                break;
            }
            if (rd_skip(r, sz) < 0 || rd_line(r, line, sizeof(line)) < 0) return -1;
            body += sz;
        }
    } else if (clen >= 0) {
        if (rd_skip(r, clen) < 0) return -1;
        body = clen;
    } else {
        body = rd_skip(r, -1);                      /* 길이 정보 없음 → 닫힐 때까지 */
        *close_after = 1;
    }
    atomic_fetch_add(&g_bytes, body);
    return code;
}

static void *worker(void *arg)
{
    (void)arg;
    rd_t *r = malloc(sizeof(rd_t));
    int fd = -1;
    for (;;) {
        long i = atomic_fetch_add(&g_next, 1);
        if (i >= g_total) break;
        double t0 = now_ms();
        int code = -1, close_after = 1;
        for (int attempt = 0; attempt < 2 && code < 0; attempt++) {   /* keep-alive가 서버 쪽에서 닫혔으면 1회 재접속 */
            if (fd < 0) {
                fd = open_conn();
                if (fd < 0) break;
                r->fd = fd; r->pos = r->len = 0;
            }
            if (write(fd, g_req, g_req_len) == (ssize_t)g_req_len) code = read_response(r, &close_after);
            if (code < 0) { close(fd); fd = -1; if (!g_keep) break; }
        }
        g_lat[i] = now_ms() - t0;
        if (code < 0) atomic_fetch_add(&g_fail, 1);
        else atomic_fetch_add(&g_codes[code / 100 < 6 ? code / 100 : 0], 1);
        if (fd >= 0 && (!g_keep || close_after)) { close(fd); fd = -1; }
    }
    if (fd >= 0) close(fd);
    free(r);
    return NULL;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    long n = 1000;
    int c = 10, opt;
    while ((opt = getopt(argc, argv, "n:c:k")) != -1) {
        if (opt == 'n') n = atol(optarg);
        else if (opt == 'c') c = atoi(optarg);
        else if (opt == 'k') g_keep = 1;
        else goto usage;
    }
    if (optind >= argc || n <= 0 || c <= 0 || strncmp(argv[optind], "http://", 7) != 0) goto usage;
    if (c > n) c = (int)n;

    /* URL 분해: http://host[:port][/path] */
    const char *u = argv[optind] + 7, *slash = strchr(u, '/');
    size_t hl = slash ? (size_t)(slash - u) : strlen(u);
    snprintf(g_host, sizeof(g_host), "%.*s", (int)hl, u);
    if (slash) snprintf(g_path, sizeof(g_path), "%s", slash);
    char *colon = strchr(g_host, ':');
    if (colon) { snprintf(g_port, sizeof(g_port), "%s", colon + 1); *colon = '\0'; }

    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_host, g_port, &hints, &g_ai) != 0) { fprintf(stderr, "주소를 찾을 수 없음: %s\n", g_host); return 1; }

    g_req_len = (size_t)snprintf(g_req, sizeof(g_req), "GET %s HTTP/1.1\r\nHost: %s:%s\r\nUser-Agent: cweb-bench\r\n%s\r\n",
                                 g_path, g_host, g_port, g_keep ? "" : "Connection: close\r\n");
    g_total = n;
    g_lat = calloc((size_t)n, sizeof(double));
    pthread_t *th = calloc((size_t)c, sizeof(pthread_t));

    double t0 = now_ms();
    for (int i = 0; i < c; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < c; i++) pthread_join(th[i], NULL);
    double sec = (now_ms() - t0) / 1000.0;

    qsort(g_lat, (size_t)n, sizeof(double), cmp_d);
    double sum = 0;
    for (long i = 0; i < n; i++) sum += g_lat[i];
    printf("대상          : %s  (동시 %d, %s)\n", argv[optind], c, g_keep ? "keep-alive" : "요청마다 새 연결");
    printf("요청          : %ld개, 실패 %ld개\n", n, (long)g_fail);
    printf("상태 코드     : 2xx %ld, 3xx %ld, 4xx %ld, 5xx %ld\n", (long)g_codes[2], (long)g_codes[3], (long)g_codes[4], (long)g_codes[5]);
    printf("걸린 시간     : %.3f초\n", sec);
    printf("초당 요청 수  : %.0f req/s\n", n / sec);
    printf("전송량        : %.2f MB/s\n", g_bytes / sec / 1048576.0);
    printf("응답 시간(ms) : 평균 %.2f | p50 %.2f | p90 %.2f | p99 %.2f | 최대 %.2f\n",
           sum / n, g_lat[n / 2], g_lat[n * 90 / 100], g_lat[n * 99 / 100], g_lat[n - 1]);
    freeaddrinfo(g_ai);
    free(g_lat);
    free(th);
    return g_fail ? 2 : 0;

usage:
    fprintf(stderr, "사용: %s -n 요청수 -c 동시연결수 [-k] http://host:port/path\n", argv[0]);
    return 1;
}
