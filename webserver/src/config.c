#include "config.h"

#include <string.h>

static int parse_port(const char *text, uint16_t *port)
{
    unsigned value = 0;

    if (*text == '\0') {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        value = value * 10 + (unsigned)(*p - '0');
        if (value > UINT16_MAX) {
            return -1;
        }
    }
    *port = (uint16_t)value;
    return 0;
}

void config_print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "사용법: %s [--port PORT] [--root DIRECTORY]\n"
            "  -p, --port PORT       수신 포트 (기본 8080, 0은 OS가 할당)\n"
            "  -r, --root DIRECTORY 정적 파일 루트 (기본 ./www)\n"
            "  -h, --help           도움말\n",
            program);
}

int config_parse(int argc, char *argv[], server_config *config)
{
    config->port = 8080;
    config->document_root = "./www";

    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        const char *value;
        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            return 1;
        }
        if (strcmp(argument, "-p") == 0 || strcmp(argument, "--port") == 0) {
            if (++i == argc) {
                fputs("포트 값이 필요합니다.\n", stderr);
                return -1;
            }
            value = argv[i];
        } else if (strncmp(argument, "--port=", 7) == 0) {
            value = argument + 7;
        } else if (strcmp(argument, "-r") == 0 || strcmp(argument, "--root") == 0) {
            if (++i == argc || *argv[i] == '\0') {
                fputs("문서 루트가 필요합니다.\n", stderr);
                return -1;
            }
            config->document_root = argv[i];
            continue;
        } else if (strncmp(argument, "--root=", 7) == 0) {
            if (argument[7] == '\0') {
                fputs("문서 루트가 필요합니다.\n", stderr);
                return -1;
            }
            config->document_root = argument + 7;
            continue;
        } else {
            fprintf(stderr, "알 수 없는 인자: %s\n", argument);
            return -1;
        }
        if (parse_port(value, &config->port) == -1) {
            fprintf(stderr, "잘못된 포트: %s (0~65535 허용)\n", value);
            return -1;
        }
    }
    return 0;
}
