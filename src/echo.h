/*
 * echo.h — /echo 경로: 받은 요청 본문을 그대로 돌려주는 시연·검증용 처리기
 *
 * 이 파일이 하는 일:
 *   POST 본문이 "빠짐없이, 순서대로" 도착했는지 눈으로 확인할 수 있게 한다.
 *   폼 데이터(application/x-www-form-urlencoded)면 "이름 = 값"으로 풀어서도 보여 준다.
 */
#ifndef ECHO_H
#define ECHO_H

#include "http.h"

#define ECHO_PATH "/echo"

/*
 * echo_serve — /echo 요청에 응답한다. POST만 허용(그 외 405).
 *   입력: fd, req(본문까지 읽힌 상태), keep_alive
 *   결과: 응답 상태 코드(로그용), 전송 실패면 -1. *sent에 보낸 본문 바이트 수
 */
int echo_serve(int fd, const http_request_t *req, int keep_alive, long long *sent);

/*
 * form_decode — 폼 값 하나를 원래 글자로 바꾼다 ('+' → 공백, %XX → 바이트)
 *   결과: 0 성공, -1 잘못된 인코딩/버퍼 부족
 */
int form_decode(const char *in, size_t n, char *out, size_t outlen);

#endif
