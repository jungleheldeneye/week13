#ifndef WEBSERVER_SERVER_H
#define WEBSERVER_SERVER_H

#include "config.h"

/* 메인이 연결을 수락하고 고정된 워커들이 처리한다. 정상 종료는 0, 오류는 -1. */
int server_run(const server_config *config);

/* 시그널 핸들러에서도 호출할 수 있다. 종료를 표시하고 알림 파이프로 메인을 깨운다. */
void server_request_stop(void);

#endif
