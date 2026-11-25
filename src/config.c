/*
 * config.c — 명령줄 인자 해석
 *
 * 이 파일이 하는 일:
 *   ./server -p 8080 -r ./www [-b 127.0.0.1] [-c routes.conf] 형태의 인자를
 *   검사하고 server_config_t로 바꾼다. 잘못된 값은 서버를 띄우기 전에 여기서 막는다.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>      /* getopt */
#include <sys/stat.h>

/* config_print_usage — 사용법을 stderr에 출력. 입력: 프로그램 이름(argv[0]) */
void config_print_usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s -p <포트> -r <정적 파일 루트> [-b <바인드 주소>] [-c <라우팅 설정 파일>]\n"
            "          [-m pool|thread|fork] [-t <워커 수>] [-f]\n"
            "  -b 생략 시 %s (모든 주소에서 접속 허용)\n"
            "  -m 동시 처리 방식: pool(기본, 스레드 풀+대기실) / thread(연결마다 스레드) / fork(연결마다 프로세스)\n"
            "  -t 스레드 풀 워커 수 (1~1024, 기본: CPU 코어 수 x 4)\n"
            "  -c Reverse Proxy 규칙 파일 (예: route /api/ 127.0.0.1:8001 127.0.0.1:8002)\n"
            "  -f Forward Proxy 허용 (GET http://호스트/경로 형태 요청을 대신 전달, 응답 캐시 포함)\n"
            "  예시: %s -p 8080 -r ./www -b 127.0.0.1 -c ./routes.conf\n",
            prog, CONFIG_DEFAULT_BIND, prog);
}

/*
 * parse_port — "8080" 같은 문자열을 포트 번호로 바꾼다.
 *   입력: 문자열   결과: 1~65535 정수, 잘못되면 -1
 *   atoi 대신 strtol을 쓰는 이유: "80a"나 "" 같은 입력을 오류로 잡아낼 수 있어서.
 */
static int parse_port(const char *s)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
        return -1;
    return (int)v;
}

const char *config_mode_name(server_mode_t m)
{
    return m == MODE_THREAD ? "thread" : m == MODE_FORK ? "fork" : "pool";
}

/* default_threads — CPU 코어 수 x 4 (4~64로 제한).
 *   x4인 이유: 워커가 디스크 읽기·네트워크 쓰기로 잠깐씩 멈추는 동안 다른 워커가 CPU를 쓰게 */
static int default_threads(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    n *= 4;
    return (int)(n < 4 ? 4 : n > 64 ? 64 : n);
}

/*
 * copy_opt — 옵션 값을 고정 크기 버퍼에 안전하게 복사한다.
 *   입력: 대상 버퍼/크기, 값   결과: 성공 0, 너무 길면 -1 (버퍼 넘침 방지)
 */
static int copy_opt(char *dst, size_t dstlen, const char *src)
{
    size_t n = strlen(src);
    if (n == 0 || n >= dstlen) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

int config_parse_args(int argc, char *argv[], server_config_t *cfg,
                      char *err, int errlen)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->bind_addr, CONFIG_DEFAULT_BIND);
    cfg->mode = MODE_POOL;
    cfg->threads = default_threads();
    const char *root = NULL;
    int opt;

    optind = 1;  /* 테스트에서 여러 번 호출해도 argv 처음부터 읽도록 초기화 */
    opterr = 0;  /* getopt 자체 오류 출력 끄기: 우리 메시지로 통일 */
    while ((opt = getopt(argc, argv, "p:r:b:c:m:t:f")) != -1) {
        switch (opt) {
        case 'p':
            cfg->port = parse_port(optarg);
            if (cfg->port < 0) {
                snprintf(err, errlen, "포트가 올바르지 않습니다: %s (1~65535)", optarg);
                return -1;
            }
            break;
        case 'r':
            root = optarg;
            break;
        case 'b':
            if (copy_opt(cfg->bind_addr, sizeof(cfg->bind_addr), optarg) != 0) {
                snprintf(err, errlen, "바인드 주소가 올바르지 않습니다");
                return -1;
            }
            break;
        case 'c':
            if (copy_opt(cfg->routes_path, sizeof(cfg->routes_path), optarg) != 0) {
                snprintf(err, errlen, "설정 파일 경로가 올바르지 않습니다");
                return -1;
            }
            break;
        case 'f':
            cfg->forward = 1;
            break;
        case 'm':
            if      (strcmp(optarg, "pool") == 0)   cfg->mode = MODE_POOL;
            else if (strcmp(optarg, "thread") == 0) cfg->mode = MODE_THREAD;
            else if (strcmp(optarg, "fork") == 0)   cfg->mode = MODE_FORK;
            else {
                snprintf(err, errlen, "-m 값은 pool, thread, fork 중 하나입니다: %s", optarg);
                return -1;
            }
            break;
        case 't': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || v < 1 || v > CONFIG_MAX_THREADS) {
                snprintf(err, errlen, "-t 값이 올바르지 않습니다: %s (1~%d)", optarg, CONFIG_MAX_THREADS);
                return -1;
            }
            cfg->threads = (int)v;
            break;
        }
        default:
            snprintf(err, errlen, "알 수 없는 옵션이거나 값이 빠졌습니다");
            return -1;
        }
    }

    if (cfg->port == 0) { snprintf(err, errlen, "-p <포트>가 필요합니다"); return -1; }
    if (root == NULL)   { snprintf(err, errlen, "-r <정적 파일 루트>가 필요합니다"); return -1; }

    /* 루트를 절대 경로로 정규화: 나중에 "../" 경로 순회 공격을 검사하는 기준점이 됨 */
    if (realpath(root, cfg->docroot) == NULL) {
        snprintf(err, errlen, "정적 파일 루트를 찾을 수 없습니다: %s", root);
        return -1;
    }
    struct stat st;
    if (stat(cfg->docroot, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, errlen, "정적 파일 루트가 폴더가 아닙니다: %s", root);
        return -1;
    }
    return 0;
}
