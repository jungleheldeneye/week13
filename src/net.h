/*
 * net.h — 소켓 준비와 "안전한" 읽기/쓰기 도구
 *
 * 이 파일이 하는 일:
 *   1) net_listen: 주소+포트로 listening 소켓을 만든다 (socket→bind→listen 묶음)
 *   2) net_write_all: 커널이 일부만 보내도 끝까지 다 보낸다 (부분 송신 대응)
 *   3) net_reader_*: 받은 바이트를 버퍼에 모아 "한 줄" 또는 "N바이트" 단위로 꺼낸다
 *      (TCP는 경계 없는 바이트 흐름이라, 한 번의 read가 요청 하나와 일치하지 않기 때문)
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>

/* net_reader 반환값 중 특수한 의미를 가진 것들 */
#define NET_ERR     (-1)   /* 소켓 오류 또는 수신 시간 초과 */
#define NET_EOF     (-2)   /* 상대가 연결을 닫음 (더 받을 데이터 없음) */
#define NET_TOOLONG (-3)   /* 한 줄이 허용 길이를 넘음 (공격/오류로 판단) */
#define NET_TIMEOUT (-4)   /* 시간 초과: 소켓 대기 시간(SO_RCVTIMEO) 또는 전체 마감 시각(deadline) 지남 */

#define NET_BUFSIZE 8192

/* 연결 하나에 붙는 수신 버퍼. 이번 read에서 남은 바이트를 다음 꺼내기까지 보관한다. */
typedef struct {
    int    fd;
    char   buf[NET_BUFSIZE];
    size_t pos;   /* 아직 안 꺼낸 데이터의 시작 위치 */
    size_t len;   /* 버퍼에 들어 있는 데이터의 끝 위치 */
    time_t deadline; /* 이 시각이 지나면 더 읽지 않고 NET_TIMEOUT (0이면 마감 없음) */
} net_reader_t;

/*
 * net_listen — host:port에서 연결을 기다리는 listening 소켓을 만든다.
 *   입력: host("0.0.0.0", "127.0.0.1", "::", "localhost" 등), port, backlog(대기열 길이),
 *         err/errlen(실패 이유)
 *   결과: listening 소켓 fd, 실패 시 -1
 */
int net_listen(const char *host, int port, int backlog, char *err, int errlen);

/*
 * net_write_all — buf의 n바이트를 전부 보낼 때까지 반복해서 write한다.
 *   결과: 0(전부 보냄), -1(오류: 상대가 끊었거나 시간 초과)
 */
int net_write_all(int fd, const void *buf, size_t n);

/*
 * net_connect — host:port에 TCP로 접속한다 (Proxy가 백엔드에 접속할 때 사용).
 *   입력: host, port, timeout_s(접속 대기 상한), err/errlen
 *   결과: 연결된 소켓 fd, 실패 시 -1 (거부됨/시간 초과/주소 해석 실패)
 *   connect는 기본으로 수십 초까지 기다릴 수 있어서, non-blocking으로 시작하고 poll로 시간을 제한한다.
 */
int net_connect(const char *host, int port, int timeout_s, char *err, int errlen);

/*
 * net_writev_all — 여러 조각(iov 배열)을 한 번의 시스템 호출로 보내고, 일부만 나가면 나머지를 이어서 보낸다.
 *   입력: fd, iov(조각 배열, 진행에 따라 내용이 바뀜), cnt(조각 수)
 *   결과: 0(전부 보냄), -1(오류)
 *   용도: 응답 헤더와 본문 첫 조각을 한 번에 보내 시스템 호출과 작은 패킷 수를 줄인다.
 */
struct iovec;
int net_writev_all(int fd, struct iovec *iov, int cnt);

/*
 * net_set_timeout — 소켓의 읽기/쓰기 대기 시간 상한을 건다.
 *   입력: fd, 초   결과: 0 성공, -1 실패
 *   이유: 연결만 열고 아무것도 안 보내는 클라이언트가 서버를 영원히 붙잡지 못하게.
 */
int net_set_timeout(int fd, int seconds);

/*
 * net_peer_str — 연결된 상대 주소를 "IP:포트" 문자열로 만든다 (로그용).
 */
void net_peer_str(const struct sockaddr *sa, socklen_t salen, char *out, size_t outlen);

/* net_reader_init — fd에 수신 버퍼를 붙인다 (버퍼 비우기) */
void net_reader_init(net_reader_t *r, int fd);

/*
 * net_reader_set_deadline — 지금부터 seconds초 안에 다 읽어야 한다는 마감을 건다 (0이면 해제).
 *   이유: SO_RCVTIMEO는 "한 번의 read"만 제한한다. 1바이트씩 9초 간격으로 보내는
 *         느린 공격(slowloris)은 매번 통과하므로, 요청 전체에 마감 시각을 따로 둔다.
 */
void net_reader_set_deadline(net_reader_t *r, int seconds);

/* net_reader_pending — 버퍼에 받아 두고 아직 안 꺼낸 바이트 수 (다음 요청이 미리 와 있는지 확인용) */
size_t net_reader_pending(const net_reader_t *r);

/*
 * net_set_nodelay — Nagle 알고리즘을 끈다 (TCP_NODELAY).
 *   Nagle: 작은 패킷을 바로 안 보내고 앞 패킷의 ACK를 기다렸다 모아 보냄.
 *   상대(클라이언트)의 "지연 ACK"(최대 40ms)와 겹치면 응답 하나에 40ms씩 멈춤.
 *   서버는 이미 writev로 모아서 보내므로 꺼도 패킷이 잘게 쪼개지지 않는다. 결과: 0/-1
 */
int net_set_nodelay(int fd);

/* net_set_nonblock — fd를 non-blocking으로 (대기 없이 바로 돌아오게). 결과: 0/-1 */
int net_set_nonblock(int fd, int on);

/*
 * net_read_line — CRLF(또는 LF)로 끝나는 한 줄을 꺼낸다.
 *   입력: r, out(줄을 담을 곳), max(out 크기)
 *   결과: 줄 길이(0 이상, 줄바꿈 문자는 빼고 '\0'으로 끝남),
 *         NET_EOF / NET_ERR / NET_TOOLONG / NET_TIMEOUT
 *   빈 줄(헤더 끝 표시)은 0을 돌려준다.
 */
ssize_t net_read_line(net_reader_t *r, char *out, size_t max);

/*
 * net_read_n — 정확히 n바이트를 꺼낸다 (요청 본문 읽기용, 4단계에서 사용).
 *   결과: n(성공), NET_EOF(중간에 끊김), NET_TIMEOUT, NET_ERR
 *   버퍼에 이미 받아 둔 바이트부터 먼저 쓰므로 헤더 뒤에 붙어 온 본문도 잃지 않는다.
 */
ssize_t net_read_n(net_reader_t *r, void *out, size_t n);

/*
 * net_read_some — 버퍼에 남은 것이 있으면 그것부터, 없으면 소켓에서 한 번 읽어 최대 max바이트를 꺼낸다.
 *   결과: 꺼낸 바이트(>0), NET_EOF, NET_TIMEOUT, NET_ERR  (Proxy가 "끝날 때까지" 중계할 때 사용)
 */
ssize_t net_read_some(net_reader_t *r, void *out, size_t max);

#endif
