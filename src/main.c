/*
 * main.c — 프로그램 시작점
 *
 * 이 파일이 하는 일:
 *   명령줄 인자를 설정으로 바꾸고(config), 설정을 출력한 뒤 서버를 실행한다(server).
 *   실제 로직은 각 모듈에 있고, main은 순서만 정한다.
 */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "server.h"
#include "proxy.h"
#include "cache.h"

int main(int argc, char *argv[])
{
    server_config_t cfg;
    char err[256];

    /* 로그가 파이프/파일로 갈 때도 줄 단위로 바로 출력되게 (오류 메시지와 순서가 섞이지 않음) */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* -h / --help: 사용법만 보여 주고 정상 종료 */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            config_print_usage(argv[0]);
            return 0;
        }

    if (config_parse_args(argc, argv, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "오류: %s\n", err);
        config_print_usage(argv[0]);
        return 1;
    }

    printf("포트: %d\n", cfg.port);
    printf("바인드 주소: %s\n", cfg.bind_addr);
    printf("정적 파일 루트: %s\n", cfg.docroot);
    if (proxy_init(cfg.routes_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "오류: %s\n", err);
        return 1;
    }
    printf("라우팅 설정: %s\n", cfg.routes_path[0] ? cfg.routes_path : "(없음)");
    for (int i = 0; i < proxy_route_count(); i++) {
        char d[512];
        proxy_describe(i, d, sizeof(d));
        printf("  - %s\n", d);
    }
    if (cfg.forward)
        printf("Forward Proxy: 켜짐 (-f), 캐시 최대 %dMB / 응답당 %dKB / 기본 %d초\n",
               CACHE_MAX_TOTAL >> 20, CACHE_MAX_OBJECT >> 10, CACHE_DEFAULT_TTL);
    else
        printf("Forward Proxy: 꺼짐\n");
    if (cfg.mode == MODE_POOL)
        printf("동시 처리: pool (워커 %d개 + poll 대기실)\n", cfg.threads);
    else
        printf("동시 처리: %s\n", config_mode_name(cfg.mode));

    int rc = server_run(&cfg);

    if (cfg.forward) {                     /* 종료 시 캐시 효과 요약 (발표/분석용) */
        long hits, misses;
        int entries;
        size_t bytes;
        cache_stats(&hits, &misses, &entries, &bytes);
        printf("[캐시] 적중 %ld번, 실패 %ld번, 저장 %d개 (%zu바이트)\n", hits, misses, entries, bytes);
    }
    cache_clear();                         /* 남은 캐시 메모리 반환 (누수 검사에 안 잡히게) */
    return rc;
}
