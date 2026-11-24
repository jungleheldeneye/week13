#include "config.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdlib.h>
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
            "  --backend IPv4:PORT  백엔드 (여러 번 지정, 최대 16개)\n"
            "  --proxy-prefix PATH 프록시 경로 접두사 (기본 /api, 전달 시 유지)\n"
            "  --proxy-timeout MS  백엔드 연결·I/O 무응답 제한 (기본 5000ms)\n"
            "  -h, --help           도움말\n",
            program);
}

static int add_backend(server_config *config, const char *value)
{
    const char *colon = strrchr(value, ':');
    backend_config backend = {0};
    struct in_addr address;
    size_t length;

    if (config->backend_count == CONFIG_MAX_BACKENDS || colon == NULL) return -1;
    length = (size_t)(colon - value);
    if (length == 0 || length >= sizeof(backend.host)) return -1;
    memcpy(backend.host, value, length);
    if (inet_pton(AF_INET, backend.host, &address) != 1 ||
        parse_port(colon + 1, &backend.port) == -1 || backend.port == 0) return -1;
    config->backends[config->backend_count++] = backend;
    return 0;
}

static bool valid_prefix(const char *path)
{
    size_t length = strlen(path);

    if (length < 2 || length > 2048 || path[0] != '/' || path[length - 1] == '/') return false;
    for (size_t i = 1; i < length; ++i) {
        unsigned char c = (unsigned char)path[i];

        if (c == '/' && path[i - 1] != '/') continue;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}

int config_parse(int argc, char *argv[], server_config *config)
{
    config->port = 8080;
    config->document_root = "./www";
    config->proxy_prefix = "/api";
    config->backend_count = 0;
    config->proxy_timeout_ms = 5000;

    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        const char *value;
        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            return 1;
        }
        if (strcmp(argument, "--backend") == 0 || strncmp(argument, "--backend=", 10) == 0) {
            if (argument[9] == '=') value = argument + 10;
            else if (++i < argc) value = argv[i];
            else return -1;
            if (add_backend(config, value) == -1) {
                fprintf(stderr, "잘못된 백엔드: %s (숫자 IPv4:1~65535, 최대 16개)\n", value);
                return -1;
            }
            continue;
        }
        if (strcmp(argument, "--proxy-prefix") == 0 || strncmp(argument, "--proxy-prefix=", 15) == 0) {
            if (argument[14] == '=') value = argument + 15;
            else if (++i < argc) value = argv[i];
            else return -1;
            if (!valid_prefix(value)) return -1;
            config->proxy_prefix = value;
            continue;
        }
        if (strcmp(argument, "--proxy-timeout") == 0 || strncmp(argument, "--proxy-timeout=", 16) == 0) {
            uint16_t milliseconds;

            if (argument[15] == '=') value = argument + 16;
            else if (++i < argc) value = argv[i];
            else return -1;
            if (parse_port(value, &milliseconds) == -1 || milliseconds == 0) return -1;
            config->proxy_timeout_ms = milliseconds;
            continue;
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
