/*
 * static_file.c — 정적 파일 응답 구현 (static_file.h 설명 참고)
 *
 * 흐름: 메서드 확인(405) → 경로 해석과 보안 검사(301/403/404) → 파일 열기
 *       → 캐시 확인(304) → 헤더 + 첫 64KB를 writev로 → 나머지를 read→write 반복
 */
#include "static_file.h"
#include "response.h"
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>    /* PATH_MAX */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* errno_to_status — 파일 관련 실패 원인을 HTTP 상태로 */
static int errno_to_status(int e)
{
    return (e == ENOENT || e == ENOTDIR || e == ENAMETOOLONG) ? 404 : 403;
}

int static_resolve(const char *docroot, const char *urlpath, char *out, size_t outlen, struct stat *st)
{
    char joined[PATH_MAX * 2];
    char real[PATH_MAX];

    /* 1) 루트 + 요청 경로를 이어 붙인다: "/Users/.../www" + "/css/a.css" */
    if (snprintf(joined, sizeof(joined), "%s%s", docroot, urlpath) >= (int)sizeof(joined))
        return 404;

    /* 2) realpath로 "..", ".", 심볼릭 링크를 모두 풀어 진짜 위치를 구한다 */
    if (realpath(joined, real) == NULL)
        return errno_to_status(errno);

    /* 3) 진짜 위치가 루트 안인지 확인 (경로 순회 공격 차단)
     *    "www" 와 "www2" 를 구분하려고 접두어 다음 글자가 '/' 또는 끝인지까지 본다 */
    size_t rl = strlen(docroot);
    if (strncmp(real, docroot, rl) != 0 || (real[rl] != '/' && real[rl] != '\0'))
        return 403;

    if (stat(real, st) != 0)
        return errno_to_status(errno);

    /* 4) 폴더면: 주소가 '/'로 끝나야 그 안의 상대 링크가 맞게 동작 → 아니면 301 */
    if (S_ISDIR(st->st_mode)) {
        size_t ul = strlen(urlpath);
        if (ul == 0 || urlpath[ul - 1] != '/') return 301;
        if (strlen(real) + sizeof("/index.html") > sizeof(real)) return 404;
        strcat(real, "/index.html");
        if (stat(real, st) != 0)
            return errno == ENOENT ? 403 : errno_to_status(errno);   /* 폴더 목록은 보여 주지 않음 */
    }

    if (!S_ISREG(st->st_mode)) return 403;   /* 장치 파일, 소켓 등은 거절 */
    if (strlen(real) >= outlen) return 404;
    strcpy(out, real);
    return 0;
}

int static_serve(int fd, const char *docroot, const http_request_t *req, int keep_alive, long long *sent)
{
    int head = (req->method == HTTP_HEAD);
    char path[PATH_MAX];
    struct stat st;
    *sent = 0;

    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
        long long n = resp_error(fd, 405, keep_alive, head, "Allow: GET, HEAD\r\n");
        *sent = n > 0 ? n : 0;
        return n < 0 ? -1 : 405;
    }

    int st_code = static_resolve(docroot, req->path, path, sizeof(path), &st);
    if (st_code == 301) {
        /* 원문 대상(인코딩 유지)에서 '?' 앞에 '/'를 끼워 넣는다: "/docs?x=1" → "/docs/?x=1" */
        char loc[HTTP_MAX_TARGET + 32];
        const char *t = req->is_absolute ? req->path : req->target;
        size_t plen = strcspn(t, "?");
        snprintf(loc, sizeof(loc), "Location: %.*s/%s\r\n", (int)plen, t, t + plen);
        long long n = resp_error(fd, 301, keep_alive, head, loc);
        *sent = n > 0 ? n : 0;
        return n < 0 ? -1 : 301;
    }
    if (st_code != 0) {
        long long n = resp_error(fd, st_code, keep_alive, head, NULL);
        *sent = n > 0 ? n : 0;
        return n < 0 ? -1 : st_code;
    }

    int f = open(path, O_RDONLY);
    if (f < 0) {
        int code = errno_to_status(errno);
        long long n = resp_error(fd, code, keep_alive, head, NULL);
        return n < 0 ? -1 : code;
    }

    /* 캐시: 브라우저가 "이 시각 이후 바뀌었으면 줘"라고 물었고 안 바뀌었으면 304 (본문 없음) */
    char lastmod[64], extra[128];
    http_date(st.st_mtime, lastmod, sizeof(lastmod));
    snprintf(extra, sizeof(extra), "Last-Modified: %s\r\n", lastmod);
    const char *ims = http_get_header(req, "If-Modified-Since");
    if (ims && strcmp(ims, lastmod) == 0) {
        close(f);
        resp_t r = { .status = 304, .extra = extra, .keep_alive = keep_alive, .head_only = 1 };
        return resp_send(fd, &r, NULL, 0) == 0 ? 304 : -1;
    }

    resp_t r = { .status = 200, .content_type = mime_type(path),
                 .content_length = (long long)st.st_size, .extra = extra,
                 .keep_alive = keep_alive, .head_only = head };

    /* 파일 하나당 64KB 버퍼. 스레드마다 따로 쓰도록 _Thread_local (5단계 대비) */
    static _Thread_local char buf[STATIC_CHUNK];
    long long remain = head ? 0 : (long long)st.st_size;

    /* 첫 조각을 읽어 헤더와 함께 writev로 보낸다 (작은 파일은 이 한 번으로 끝) */
    ssize_t k = 0;
    if (remain > 0) {
        k = read(f, buf, sizeof(buf));
        if (k < 0) { close(f); return -1; }   /* 헤더 보내기 전 실패지만 단순하게 연결 종료 */
    }
    if (resp_send(fd, &r, buf, (size_t)k) != 0) { close(f); return -1; }
    remain -= k;
    *sent = k;

    /* 나머지: read → write 반복. 파일이 전송 중 줄어들면 멈춘다 (길이 약속은 깨지므로 연결 종료) */
    while (remain > 0) {
        k = read(f, buf, sizeof(buf));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { close(f); return -1; }
        if (net_write_all(fd, buf, (size_t)k) != 0) { close(f); return -1; }
        remain -= k;
        *sent += k;
    }
    close(f);
    return 200;
}
