#ifndef WEBSERVER_CONFIG_H
#define WEBSERVER_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define CONFIG_MAX_BACKENDS 16

typedef struct {
    char host[16]; /* 숫자 IPv4 주소 */
    uint16_t port;
} backend_config;

typedef struct {
    uint16_t port;
    const char *document_root;
    const char *proxy_prefix;
    backend_config backends[CONFIG_MAX_BACKENDS];
    size_t backend_count;
    unsigned proxy_timeout_ms;
} server_config;

/* 0: 정상, 1: 도움말 요청, -1: 잘못된 인자. 문자열은 argv를 참조한다. */
int config_parse(int argc, char *argv[], server_config *config);
void config_print_usage(FILE *stream, const char *program);

#endif
