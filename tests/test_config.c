/*
 * test_config.c — 명령줄 인자 해석(config) 테스트
 *   정상 입력, 필수 옵션 누락, 잘못된 포트, 없는 폴더, -b 기본값/지정값을 확인한다.
 */
#include "test.h"
#include "../src/config.h"

/* parse — 테스트용 래퍼: 오류 메시지는 버리고 결과 코드만 돌려준다 */
static int parse(int argc, char **argv, server_config_t *cfg)
{
    char err[256];
    return config_parse_args(argc, argv, cfg, err, sizeof(err));
}

void test_config(void)
{
    server_config_t cfg;

    char *ok[] = {"server", "-p", "8080", "-r", "www", "-c", "routes.conf"};
    CHECK(parse(7, ok, &cfg) == 0);
    CHECK(cfg.port == 8080);
    CHECK(cfg.docroot[0] == '/');                 /* 절대 경로로 바뀜 */
    CHECK_STR(cfg.routes_path, "routes.conf");
    CHECK_STR(cfg.bind_addr, "0.0.0.0");          /* -b 생략 시 기본값 */

    char *bind[] = {"server", "-p", "8080", "-r", "www", "-b", "127.0.0.1"};
    CHECK(parse(7, bind, &cfg) == 0);
    CHECK_STR(cfg.bind_addr, "127.0.0.1");

    char *no_port[] = {"server", "-r", "www"};
    CHECK(parse(3, no_port, &cfg) == -1);

    char *bad_port[] = {"server", "-p", "70000", "-r", "www"};
    CHECK(parse(5, bad_port, &cfg) == -1);

    char *word_port[] = {"server", "-p", "80a", "-r", "www"};
    CHECK(parse(5, word_port, &cfg) == -1);

    char *no_root[] = {"server", "-p", "8080", "-r", "없는폴더"};
    CHECK(parse(5, no_root, &cfg) == -1);

    CHECK(cfg.mode == MODE_POOL);                 /* -m 생략 시 스레드 풀 */
    CHECK(cfg.threads >= 4 && cfg.threads <= 64); /* 기본 워커 수 범위 */

    char *mode[] = {"server", "-p", "8080", "-r", "www", "-m", "fork", "-t", "16"};
    CHECK(parse(9, mode, &cfg) == 0);
    CHECK(cfg.mode == MODE_FORK && cfg.threads == 16);

    char *bad_mode[] = {"server", "-p", "8080", "-r", "www", "-m", "epoll"};
    CHECK(parse(7, bad_mode, &cfg) == -1);
    char *bad_t[] = {"server", "-p", "8080", "-r", "www", "-t", "0"};
    CHECK(parse(7, bad_t, &cfg) == -1);

    char *missing_val[] = {"server", "-p", "8080", "-r", "www", "-b"};
    CHECK(parse(6, missing_val, &cfg) == -1);     /* -b 뒤에 값이 없음 */
}
