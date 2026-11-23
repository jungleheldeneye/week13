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
    puts("PASS: CLI defaults, long/short/equal options, help, missing values and invalid ports");
    return EXIT_SUCCESS;
}
