/*
 * static_file.h — 정적 파일 응답
 *
 * 이 파일이 하는 일:
 *   요청 경로를 문서 루트 안의 실제 파일로 바꾸고(보안 검사 포함),
 *   그 파일을 상태 줄 + 헤더 + 내용으로 보낸다.
 */
#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <stddef.h>
#include <sys/stat.h>
#include "http.h"

#define STATIC_CHUNK 65536   /* 파일을 읽어 보내는 단위 (64KB) */

/*
 * static_resolve — URL 경로를 루트 안의 실제 파일 절대 경로로 바꾼다.
 *   입력: docroot(절대 경로), urlpath(해석된 요청 경로, "/css/a.css"), out/outlen, st(파일 정보)
 *   결과: 0     = 보낼 파일을 찾음 (out, st 채워짐)
 *         301   = 폴더인데 끝에 '/'가 없음 → '/'를 붙인 주소로 이동시켜야 함
 *         403   = 루트 밖이거나, 읽을 수 없거나, 일반 파일이 아님
 *         404   = 없음
 */
int static_resolve(const char *docroot, const char *urlpath, char *out, size_t outlen, struct stat *st);

/*
 * static_serve — GET/HEAD 요청에 정적 파일로 응답한다.
 *   입력: fd, docroot, req, keep_alive
 *   결과: 응답 상태 코드(200/304/404 등, 로그용). 전송 중 실패면 -1 (연결을 닫아야 함)
 *   *sent에는 보낸 본문 바이트 수를 적는다 (로그용).
 */
int static_serve(int fd, const char *docroot, const http_request_t *req, int keep_alive, long long *sent);

#endif
