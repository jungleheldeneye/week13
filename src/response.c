/*
 * response.c — HTTP 응답 만들기와 보내기 (response.h 설명 참고)
 */
#include "response.h"
#include "http.h"
#include "net.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/uio.h>

void http_date(time_t t, char *out, size_t outlen)
{
    struct tm tm;
    gmtime_r(&t, &tm);   /* HTTP 날짜는 항상 GMT(UTC). _r 버전은 스레드에 안전 */
    /* strftime의 %a/%b는 지역 설정에 따라 바뀔 수 있어서 영어 이름을 직접 쓴다 */
    static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(out, outlen, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             days[tm.tm_wday], tm.tm_mday, mons[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

const char *mime_type(const char *path)
{
    static const struct { const char *ext, *type; } table[] = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".css",  "text/css; charset=utf-8"},  {".js",  "text/javascript; charset=utf-8"},
        {".json", "application/json"},         {".txt", "text/plain; charset=utf-8"},
        {".xml",  "application/xml"},          {".svg", "image/svg+xml"},
        {".png",  "image/png"},  {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},  {".webp", "image/webp"}, {".ico", "image/x-icon"},
        {".pdf",  "application/pdf"}, {".wasm", "application/wasm"},
        {".mp4",  "video/mp4"},  {".mp3", "audio/mpeg"},
        {".woff", "font/woff"},  {".woff2", "font/woff2"},
    };
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash))                    /* "/a.b/c" 의 ".b/c"는 확장자 아님 */
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
            if (strcasecmp(dot, table[i].ext) == 0) return table[i].type;
    return "application/octet-stream";                     /* 모르면 "그냥 바이트" → 다운로드 */
}

int resp_send(int fd, const resp_t *r, const void *body, size_t blen)
{
    char head[1024], date[64];
    http_date(time(NULL), date, sizeof(date));

    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: " SERVER_NAME "\r\n",
                     r->status, http_reason(r->status), date);
    if (r->content_type)
        n += snprintf(head + n, sizeof(head) - (size_t)n, "Content-Type: %s\r\n", r->content_type);
    if (r->status != 304)   /* 304는 본문이 없어서 길이를 적지 않는다 */
        n += snprintf(head + n, sizeof(head) - (size_t)n, "Content-Length: %lld\r\n", r->content_length);
    if (r->keep_alive)
        n += snprintf(head + n, sizeof(head) - (size_t)n,
                      "Connection: keep-alive\r\nKeep-Alive: timeout=5, max=100\r\n");
    else
        n += snprintf(head + n, sizeof(head) - (size_t)n, "Connection: close\r\n");
    if (r->extra)
        n += snprintf(head + n, sizeof(head) - (size_t)n, "%s", r->extra);
    n += snprintf(head + n, sizeof(head) - (size_t)n, "\r\n");   /* 빈 줄 = 헤더 끝 */
    if (n <= 0 || (size_t)n >= sizeof(head)) return -1;

    /* writev: 헤더와 본문 조각을 메모리에서 합치지 않고도 한 번에 보낸다 */
    struct iovec iov[2] = {
        { .iov_base = head, .iov_len = (size_t)n },
        { .iov_base = (void *)body, .iov_len = r->head_only ? 0 : blen },
    };
    return net_writev_all(fd, iov, iov[1].iov_len ? 2 : 1);
}

long long resp_error(int fd, int status, int keep_alive, int head_only, const char *extra)
{
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "<!doctype html><html><head><meta charset=\"utf-8\">"
                     "<title>%d %s</title></head><body><h1>%d %s</h1>"
                     "<hr><p>" SERVER_NAME "</p></body></html>\n",
                     status, http_reason(status), status, http_reason(status));
    resp_t r = { .status = status, .content_type = "text/html; charset=utf-8",
                 .content_length = n, .extra = extra,
                 .keep_alive = keep_alive, .head_only = head_only };
    return resp_send(fd, &r, body, (size_t)n) == 0 ? n : -1;
}
