/*
 * echo.c — /echo 처리기 구현 (echo.h 설명 참고)
 *
 * 응답 본문 예:
 *   받은 본문: 23바이트 (Content-Length 방식)
 *   Content-Type: application/x-www-form-urlencoded
 *   --- 원문 ---
 *   name=hyeok&msg=hi+there
 *   --- 폼 해석 ---
 *   name = hyeok
 *   msg = hi there
 */
#include "echo.h"
#include "response.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int form_decode(const char *in, size_t n, char *out, size_t outlen)
{
    char tmp[HTTP_MAX_TARGET];
    if (n >= sizeof(tmp)) return -1;
    for (size_t i = 0; i < n; i++)          /* 폼에서는 공백이 '+'로 온다 */
        tmp[i] = in[i] == '+' ? ' ' : in[i];
    return http_url_decode(tmp, n, out, outlen);
}

/* append — 늘어나는 출력 버퍼 뒤에 바이트를 붙인다. 결과: 0 성공, -1 메모리 부족 */
static int append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 1024;
        while (nc < *len + n + 1) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

int echo_serve(int fd, const http_request_t *req, int keep_alive, long long *sent)
{
    *sent = 0;
    if (req->method != HTTP_POST) {
        long long n = resp_error(fd, 405, keep_alive, req->method == HTTP_HEAD, "Allow: POST\r\n");
        if (n > 0) *sent = n;
        return n < 0 ? -1 : 405;
    }

    char *out = NULL;
    size_t len = 0, cap = 0;
    char line[512];
    const char *ctype = http_get_header(req, "Content-Type");

    int k = snprintf(line, sizeof(line), "받은 본문: %zu바이트 (%s 방식)\nContent-Type: %s\n--- 원문 ---\n",
                     req->body_len, req->chunked ? "chunked" : "Content-Length",
                     ctype ? ctype : "(없음)");
    int ok = append(&out, &len, &cap, line, (size_t)k) == 0 &&
             append(&out, &len, &cap, req->body ? req->body : "", req->body_len) == 0;

    /* 폼 데이터면 "a=1&b=2"를 쪼개서 이름 = 값으로 보여 준다 */
    if (ok && ctype && strncasecmp(ctype, "application/x-www-form-urlencoded", 33) == 0 && req->body) {
        ok = append(&out, &len, &cap, "\n--- 폼 해석 ---\n", strlen("\n--- 폼 해석 ---\n")) == 0;
        const char *p = req->body, *end = req->body + req->body_len;
        while (ok && p < end) {
            const char *amp = memchr(p, '&', (size_t)(end - p));
            const char *pe = amp ? amp : end;
            const char *eq = memchr(p, '=', (size_t)(pe - p));
            const char *ne = eq ? eq : pe;
            char name[256], val[HTTP_MAX_TARGET];
            if (form_decode(p, (size_t)(ne - p), name, sizeof(name)) != 0) strcpy(name, "(해석 불가)");
            if (!eq) val[0] = '\0';
            else if (form_decode(eq + 1, (size_t)(pe - eq - 1), val, sizeof(val)) != 0) strcpy(val, "(해석 불가)");
            k = snprintf(line, sizeof(line), "%s = ", name);
            ok = append(&out, &len, &cap, line, (size_t)k) == 0 &&
                 append(&out, &len, &cap, val, strlen(val)) == 0 &&
                 append(&out, &len, &cap, "\n", 1) == 0;
            p = pe + 1;
        }
    }
    if (!ok) {
        free(out);
        long long n = resp_error(fd, 500, keep_alive, 0, NULL);
        return n < 0 ? -1 : 500;
    }

    resp_t r = { .status = 200, .content_type = "text/plain; charset=utf-8",
                 .content_length = (long long)len, .keep_alive = keep_alive };
    int rc = resp_send(fd, &r, out, len);
    free(out);
    if (rc != 0) return -1;
    *sent = (long long)len;
    return 200;
}
