/*
 * test_main.c — 모든 테스트 묶음을 실행하고 결과를 요약한다 (make test)
 *   실패가 하나라도 있으면 종료 코드 1 → make test가 실패로 표시됨
 */
#include "test.h"

int t_run = 0, t_fail = 0;

void test_config(void);
void test_net(void);
void test_http(void);
void test_static(void);
void test_http_body(void);
void test_proxy(void);
void test_cache(void);

int main(void)
{
    RUN(test_config);
    RUN(test_net);
    RUN(test_http);
    RUN(test_http_body);
    RUN(test_static);
    RUN(test_proxy);
    RUN(test_cache);
    printf("\n%d개 검사 중 %d개 실패\n", t_run, t_fail);
    return t_fail ? 1 : 0;
}
