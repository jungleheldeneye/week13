/*
 * test_net.c — net 모듈 테스트
 *   socketpair(서로 연결된 소켓 두 개)로 "네트워크 없이" 송수신을 흉내 내서
 *   - 한 줄이 여러 조각으로 도착해도 제대로 합쳐지는지 (TCP 바이트 흐름)
 *   - 헤더 뒤에 붙어 온 본문 바이트를 잃지 않는지
 *   - 너무 긴 줄, 중간 끊김을 구분하는지
 *   그리고 실제 net_listen → connect → accept 가 되는지 확인한다.
 */
#include "test.h"
#include "../src/net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

static void test_reader_pieces(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* 한 요청을 일부러 이상한 지점에서 잘라 세 번에 나눠 보낸다 */
    const char *p1 = "GET /index.html HT";
    const char *p2 = "TP/1.1\r\nHost: a\r\n\r";
    const char *p3 = "\nBODY";
    CHECK(net_write_all(sv[1], p1, strlen(p1)) == 0);
    CHECK(net_write_all(sv[1], p2, strlen(p2)) == 0);
    CHECK(net_write_all(sv[1], p3, strlen(p3)) == 0);
    close(sv[1]);

    net_reader_t r;
    char line[64], body[8];
    net_reader_init(&r, sv[0]);
    CHECK(net_read_line(&r, line, sizeof(line)) == 24);
    CHECK_STR(line, "GET /index.html HTTP/1.1");
    CHECK(net_read_line(&r, line, sizeof(line)) == 7);
    CHECK_STR(line, "Host: a");
    CHECK(net_read_line(&r, line, sizeof(line)) == 0);   /* 빈 줄 = 헤더 끝 */
    CHECK(net_read_n(&r, body, 4) == 4);                 /* 남아 있던 본문 */
    CHECK(memcmp(body, "BODY", 4) == 0);
    CHECK(net_read_line(&r, line, sizeof(line)) == NET_EOF);
    close(sv[0]);
}

static void test_reader_limits(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *msg = "0123456789ABCDEF\r\nhalf";
    CHECK(net_write_all(sv[1], msg, strlen(msg)) == 0);
    close(sv[1]);

    net_reader_t r;
    char small[8], line[64];
    net_reader_init(&r, sv[0]);
    CHECK(net_read_line(&r, small, sizeof(small)) == NET_TOOLONG);  /* 8바이트 칸에 16글자 */

    /* 거절된 줄은 버퍼에 그대로 남아 있음 → 큰 칸으로 다시 읽으면 성공 */
    CHECK(net_read_line(&r, line, sizeof(line)) == 16);
    CHECK(net_read_line(&r, line, sizeof(line)) == NET_ERR);        /* "half"에서 끊김 */
    close(sv[0]);
}

static void test_listen_accept(void)
{
    char err[256];
    int lfd = net_listen("127.0.0.1", 18080, 8, err, sizeof(err));
    CHECK(lfd >= 0);
    if (lfd < 0) { printf("  %s\n", err); return; }

    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(18080);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == 0);   /* 3-way handshake 완료 */

    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    int s = accept(lfd, (struct sockaddr *)&ss, &sl);           /* 대기열에서 꺼냄 */
    CHECK(s >= 0);

    char peer[128];
    net_peer_str((struct sockaddr *)&ss, sl, peer, sizeof(peer));
    CHECK(strncmp(peer, "127.0.0.1:", 10) == 0);

    /* 같은 포트를 또 열면 실패해야 한다 (이미 사용 중) */
    int dup = net_listen("127.0.0.1", 18080, 8, err, sizeof(err));
    CHECK(dup < 0);
    if (dup >= 0) close(dup);

    /* 존재하지 않는 주소는 해석 단계에서 실패 */
    CHECK(net_listen("no.such.host.invalid", 18081, 8, err, sizeof(err)) < 0);

    close(c); close(s); close(lfd);
}

void test_net(void)
{
    test_reader_pieces();
    test_reader_limits();
    test_listen_accept();
}
