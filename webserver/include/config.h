#ifndef WEBSERVER_CONFIG_H
#define WEBSERVER_CONFIG_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint16_t port;
    const char *document_root;
} server_config;

/* 0: 정상, 1: 도움말 요청, -1: 잘못된 인자. 문자열은 argv를 참조한다. */
int config_parse(int argc, char *argv[], server_config *config);
void config_print_usage(FILE *stream, const char *program);

#endif
