/*
 * net.c — 소켓 준비와 안전한 읽기/쓰기 구현 (net.h 설명 참고)
 *
 * 핵심 아이디어:
 *   - getaddrinfo로 주소를 해석하므로 IPv4/IPv6/호스트 이름을 한 코드로 처리한다.
 *   - read/write는 "요청한 만큼" 처리된다는 보장이 없다. 그래서 반복(write_all)과
 *     버퍼링(reader)으로 감싸서, 위쪽 HTTP 코드는 줄/바이트 단위로만 생각하면 되게 한다.
 */
#include "net.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/uio.h>    /* writev, struct iovec */
#include <netinet/in.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <arpa/inet.h>

int net_listen(const char *host, int port, int backlog, char *err, int errlen)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1, rc, last_errno = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;    /* IPv4든 IPv6든 주소에 맞게 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */
    hints.ai_flags    = AI_PASSIVE;   /* "서버용(bind용) 주소"로 달라는 뜻 */
    snprintf(portstr, sizeof(portstr), "%d", port);

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "주소 해석 실패 %s: %s", host, gai_strerror(rc));
        return -1;
    }

    /* 후보 주소가 여러 개일 수 있음(예: localhost → ::1, 127.0.0.1). 처음 성공하는 것을 쓴다 */
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last_errno = errno; continue; }

        /* 서버를 껐다 바로 켤 때 TIME_WAIT 때문에 "Address already in use"가 나는 것 방지 */
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, backlog) == 0)
            break;                      /* 성공: fd를 들고 루프 탈출 */

        last_errno = errno;
        close(fd);                      /* 실패한 후보의 fd는 바로 반납 (fd 누수 방지) */
        fd = -1;
    }
    freeaddrinfo(res);                  /* getaddrinfo가 만든 목록 메모리 반납 */

    if (fd < 0)
        snprintf(err, errlen, "%s:%d 열기 실패: %s", host, port, strerror(last_errno));
    return fd;
}

int net_connect(const char *host, int port, int timeout_s, char *err, int errlen)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1, last = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    int rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "주소 해석 실패 %s: %s", host, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last = errno; continue; }
        fcntl(fd, F_SETFD, FD_CLOEXEC);          /* CGI 자식에게 새지 않게 */
        net_set_nonblock(fd, 1);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;   /* 바로 연결됨 (같은 컴퓨터) */
        if (errno == EINPROGRESS) {
            /* 3-way handshake 진행 중: 쓰기 가능해질 때까지(=연결 완료 또는 실패) 최대 timeout_s 대기 */
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            int n;
            do { n = poll(&p, 1, timeout_s * 1000); } while (n < 0 && errno == EINTR);
            if (n == 1) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);   /* 실제 결과 확인 */
                if (soerr == 0) break;
                last = soerr;
            } else {
                last = n == 0 ? ETIMEDOUT : errno;
            }
        } else {
            last = errno;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        snprintf(err, errlen, "%s:%d 접속 실패: %s", host, port, strerror(last));
        errno = last;
        return -1;
    }
    net_set_nonblock(fd, 0);                     /* 이후 읽기/쓰기는 시간 제한을 건 blocking 방식 */
    return fd;
}

int net_write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;   /* 시그널에 끊긴 것뿐: 다시 시도 */
            return -1;                       /* EPIPE(상대가 끊음), EAGAIN(시간 초과) 등 */
        }
        p += w;                              /* 보낸 만큼 앞으로 이동하고 나머지를 다시 보냄 */
        n -= (size_t)w;
    }
    return 0;
}

int net_writev_all(int fd, struct iovec *iov, int cnt)
{
    while (cnt > 0) {
        ssize_t w = writev(fd, iov, cnt);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        /* 보낸 바이트만큼 앞 조각들을 소비: 다 보낸 조각은 건너뛰고, 걸친 조각은 시작점을 옮긴다 */
        size_t done = (size_t)w;
        while (cnt > 0 && done >= iov->iov_len) {
            done -= iov->iov_len;
            iov++;
            cnt--;
        }
        if (cnt > 0) {
            iov->iov_base = (char *)iov->iov_base + done;
            iov->iov_len -= done;
        }
    }
    return 0;
}

int net_set_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return -1;
    return 0;
}

void net_peer_str(const struct sockaddr *sa, socklen_t salen, char *out, size_t outlen)
{
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    /* NUMERICHOST: DNS 역조회를 하지 않음 (느려지고 로그용으로는 IP면 충분) */
    if (getnameinfo(sa, salen, host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0)
        snprintf(out, outlen, "%s:%s", host, serv);
    else
        snprintf(out, outlen, "(알 수 없음)");
}

void net_reader_init(net_reader_t *r, int fd)
{
    r->fd = fd;
    r->pos = r->len = 0;
    r->deadline = 0;
}

size_t net_reader_pending(const net_reader_t *r)
{
    return r->len - r->pos;
}

int net_set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

void net_reader_set_deadline(net_reader_t *r, int seconds)
{
    r->deadline = seconds > 0 ? time(NULL) + seconds : 0;
}

/* sock_read — 마감 확인 + EINTR 재시도 + 시간 초과 구분을 한곳에 모은 read (내부용)
 *   결과: 읽은 바이트(>0), NET_EOF, NET_TIMEOUT, NET_ERR */
static ssize_t sock_read(net_reader_t *r, void *dst, size_t n)
{
    for (;;) {
        if (r->deadline && time(NULL) >= r->deadline) return NET_TIMEOUT;
        ssize_t k = read(r->fd, dst, n);
        if (k > 0) return k;
        if (k == 0) return NET_EOF;                         /* 상대가 FIN을 보냄 */
        if (errno == EINTR) continue;                       /* 시그널에 잠깐 끊김: 다시 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) return NET_TIMEOUT;  /* SO_RCVTIMEO 지남 */
        return NET_ERR;
    }
}

/*
 * fill — 버퍼가 비었을 때 소켓에서 한 번 더 읽어 채운다 (내부용).
 *   결과: 읽은 바이트 수(>0), NET_EOF, NET_TIMEOUT, NET_ERR
 */
static ssize_t fill(net_reader_t *r)
{
    if (r->pos > 0 && r->pos < r->len) {
        /* 남은 데이터를 맨 앞으로 당겨서 뒤쪽 빈 공간을 확보 */
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
    }
    r->len -= r->pos;
    r->pos = 0;
    if (r->len == sizeof(r->buf)) return NET_TOOLONG;  /* 버퍼가 꽉 찼는데 더 필요함 */

    ssize_t n = sock_read(r, r->buf + r->len, sizeof(r->buf) - r->len);
    if (n > 0) r->len += (size_t)n;
    return n;
}

ssize_t net_read_line(net_reader_t *r, char *out, size_t max)
{
    for (;;) {
        /* 1) 이미 받아 둔 데이터 안에 '\n'이 있으면 거기까지가 한 줄 */
        char *nl = memchr(r->buf + r->pos, '\n', r->len - r->pos);
        if (nl != NULL) {
            size_t linelen = (size_t)(nl - (r->buf + r->pos));
            size_t datalen = linelen;
            if (datalen > 0 && r->buf[r->pos + datalen - 1] == '\r')
                datalen--;                              /* CRLF의 CR 제거 */
            if (datalen + 1 > max) return NET_TOOLONG;
            memcpy(out, r->buf + r->pos, datalen);
            out[datalen] = '\0';
            r->pos += linelen + 1;                      /* '\n'까지 소비, 나머지는 다음을 위해 남김 */
            return (ssize_t)datalen;
        }
        /* 2) 아직 줄 끝이 안 왔는데 이미 max를 넘었으면 더 기다릴 필요 없이 거절 */
        if (r->len - r->pos >= max) return NET_TOOLONG;

        /* 3) 줄 끝이 올 때까지 더 받는다 (TCP는 한 줄이 여러 조각으로 올 수 있음) */
        ssize_t n = fill(r);
        if (n == NET_EOF && r->len > 0) return NET_ERR; /* 줄 중간에 끊김 */
        if (n < 0) return n;
    }
}

ssize_t net_read_n(net_reader_t *r, void *out, size_t n)
{
    char *dst = out;
    size_t got = 0;

    /* 먼저 버퍼에 남아 있는 바이트(헤더와 같이 도착한 본문 일부)를 쓴다 */
    size_t avail = r->len - r->pos;
    if (avail > 0) {
        size_t take = avail < n ? avail : n;
        memcpy(dst, r->buf + r->pos, take);
        r->pos += take;
        got += take;
    }
    /* 나머지는 소켓에서 직접 받는다 (큰 본문을 버퍼에 두 번 복사하지 않기 위해) */
    while (got < n) {
        ssize_t k = sock_read(r, dst + got, n - got);
        if (k < 0) return k;
        got += (size_t)k;
    }
    return (ssize_t)got;
}

ssize_t net_read_some(net_reader_t *r, void *out, size_t max)
{
    size_t avail = r->len - r->pos;
    if (avail > 0) {
        size_t take = avail < max ? avail : max;
        memcpy(out, r->buf + r->pos, take);
        r->pos += take;
        return (ssize_t)take;
    }
    return sock_read(r, out, max);
}

int net_set_nodelay(int fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}
