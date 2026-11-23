#include "server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static void handle_shutdown(int signal_number)
{
    (void)signal_number;
    server_request_stop();
}

static int install_signal_handlers(void)
{
    struct sigaction action = {0};

    action.sa_handler = handle_shutdown;
    /* SIGINT와 SIGTERM 핸들러가 서로 중첩되지 않게 한다. */
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    /* 종료 신호는 메인에 알림을 보내며, SA_RESTART로 대기를 재개하지 않는다. */
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) == -1) {
        perror("sigaction SIGPIPE");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    server_config config;
    int parsed = config_parse(argc, argv, &config);

    if (parsed != 0) {
        config_print_usage(parsed > 0 ? stdout : stderr, argv[0]);
        return parsed > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    /* 터미널뿐 아니라 출력 리다이렉션 중에도 접속 로그를 바로 표시한다. */
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        fputs("로그 출력 설정 실패\n", stderr);
        return EXIT_FAILURE;
    }
    if (install_signal_handlers() == -1) {
        return EXIT_FAILURE;
    }

    return server_run(&config) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
