#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void expect(char **arguments, int count, int expected,
                   unsigned port, const char *root)
{
    server_config config;

    CHECK(config_parse(count, arguments, &config) == expected);
    if (expected == 0) {
        CHECK(config.port == port);
        CHECK(strcmp(config.document_root, root) == 0);
        CHECK(strcmp(config.proxy_prefix, "/api") == 0);
        CHECK(config.backend_count == 0 && config.proxy_timeout_ms == 5000);
    }
}

int main(void)
{
    char *defaults[] = {"webserver"};
    char *long_options[] = {"webserver", "--port", "9000", "--root", "/tmp/site"};
    char *short_options[] = {"webserver", "-r", "www", "-p", "65535"};
    char *equal_options[] = {"webserver", "--port=0", "--root=www"};
    char *help[] = {"webserver", "--help"};
    char *missing_port[] = {"webserver", "--port"};
    char *missing_root[] = {"webserver", "--root"};
    char *empty_root[] = {"webserver", "--root="};
    char *unknown[] = {"webserver", "--unknown"};
    const char *bad_ports[] = {"", "-1", "+1", "65536", "999999999999999999999", "80x", " 80"};

    expect(defaults, 1, 0, 8080, "./www");
    expect(long_options, 5, 0, 9000, "/tmp/site");
    expect(short_options, 5, 0, 65535, "www");
    expect(equal_options, 3, 0, 0, "www");
    expect(help, 2, 1, 0, "");
    expect(missing_port, 2, -1, 0, "");
    expect(missing_root, 2, -1, 0, "");
    expect(empty_root, 2, -1, 0, "");
    expect(unknown, 2, -1, 0, "");
    for (size_t i = 0; i < sizeof(bad_ports) / sizeof(bad_ports[0]); ++i) {
        char *arguments[] = {"webserver", "--port", (char *)bad_ports[i]};

        expect(arguments, 3, -1, 0, "");
    }
    {
        server_config config;
        char *options[] = {"webserver", "--backend", "127.0.0.1:8001", "--backend=127.0.0.2:8002",
                           "--proxy-prefix", "/v1/api", "--proxy-timeout=250"};
        const char *bad_backends[] = {"", "localhost:8001", "127.0.0.1", "127.0.0.1:0",
                                     "127.0.0.1:65536", "256.1.1.1:80", "127.0.0.1:abc", "[::1]:80"};
        const char *bad_prefixes[] = {"", "/", "api", "/api/", "//api", "/a/../api", "/api?x", "/a%2fb"};

        CHECK(config_parse(7, options, &config) == 0);
        CHECK(config.backend_count == 2 && config.backends[0].port == 8001 && config.backends[1].port == 8002);
        CHECK(strcmp(config.backends[1].host, "127.0.0.2") == 0);
        CHECK(strcmp(config.proxy_prefix, "/v1/api") == 0 && config.proxy_timeout_ms == 250);
        for (size_t i = 0; i < sizeof(bad_backends) / sizeof(bad_backends[0]); ++i) {
            char *args[] = {"webserver", "--backend", (char *)bad_backends[i]};
            CHECK(config_parse(3, args, &config) == -1);
        }
        for (size_t i = 0; i < sizeof(bad_prefixes) / sizeof(bad_prefixes[0]); ++i) {
            char *args[] = {"webserver", "--proxy-prefix", (char *)bad_prefixes[i]};
            CHECK(config_parse(3, args, &config) == -1);
        }
        for (size_t i = 0; i < sizeof(bad_ports) / sizeof(bad_ports[0]); ++i) {
            char *args[] = {"webserver", "--proxy-timeout", (char *)bad_ports[i]};
            CHECK(config_parse(3, args, &config) == -1);
        }
        char *zero[] = {"webserver", "--proxy-timeout=0"};
        CHECK(config_parse(2, zero, &config) == -1);
        char *missing[] = {"webserver", "--backend"};
        CHECK(config_parse(2, missing, &config) == -1);
        missing[1] = "--proxy-prefix";
        CHECK(config_parse(2, missing, &config) == -1);
        missing[1] = "--proxy-timeout";
        CHECK(config_parse(2, missing, &config) == -1);
        char *many[CONFIG_MAX_BACKENDS + 2] = {"webserver"};
        for (size_t i = 1; i < CONFIG_MAX_BACKENDS + 2; ++i) many[i] = "--backend=127.0.0.1:8001";
        CHECK(config_parse(CONFIG_MAX_BACKENDS + 2, many, &config) == -1);
    }
    puts("PASS: CLI defaults, long/short/equal options, help, missing values and invalid ports");
    return EXIT_SUCCESS;
}
