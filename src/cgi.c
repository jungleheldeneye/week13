/*
 * cgi.c — CGI 실행 구현 (cgi.h 설명 참고)
 *
 * 흐름:
 *   1) 스크립트 찾기: 경로를 앞에서부터 잘라 보며 "실제 파일"이 나오는 지점을 찾음 (뒤는 PATH_INFO)
 *      → 루트의 cgi-bin 폴더 밖이면 403, 없으면 404, 실행 권한 없으면 403
 *   2) 환경 변수 준비 (fork 전에 다 만들어 둠: 자식에서는 안전한 함수만 쓰기 위해)
 *   3) 파이프 두 개 + fork → 자식: 표준 입출력 연결, 다른 fd 정리, execve
 *   4) 부모: poll로 "본문 쓰기"와 "출력 읽기"를 동시에 (한쪽만 하다 서로 기다리며 멈추는 것 방지)
 *      → 출력의 헤더 부분을 해석해 상태 줄을 만들고, 본문은 chunked로 흘려보냄
 *   5) 시간 초과(10초)면 SIGKILL, 마지막에 waitpid로 자식 회수 (좀비 방지)
 */
#include "cgi.h"
#include "net.h"
#include "response.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/uio.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CGI_MAX_ENV 128

int cgi_is_request(const http_request_t *req)
{
    return strncmp(req->path, CGI_PREFIX, strlen(CGI_PREFIX)) == 0;
}

/* fail — 아직 아무것도 안 보냈을 때 오류 응답 */
static int fail(int fd, int status, const char *why, int *keep_out, long long *sent)
{
    *keep_out = 0;
    if (why) fprintf(stderr, "[CGI] %d: %s\n", status, why);
    long long n = resp_error(fd, status, 0, 0, NULL);
    *sent = n > 0 ? n : 0;
    return n < 0 ? -1 : status;
}

/*
 * find_script — URL 경로에서 실행할 스크립트와 PATH_INFO를 찾는다.
 *   결과: 0 성공(script=절대 경로, script_name="/cgi-bin/a.sh", path_info="/x/y"), 또는 403/404
 */
static int find_script(const char *docroot, const char *path, char *script, char *script_name, char *path_info)
{
    char cgidir[PATH_MAX], tmp[PATH_MAX * 2], real[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/cgi-bin", docroot);
    if (realpath(tmp, cgidir) == NULL) return 404;

    size_t plen = strlen(path);
    size_t pos = strlen(CGI_PREFIX);
    while (pos <= plen) {
        size_t end = pos + strcspn(path + pos, "/");     /* 다음 '/' 까지가 후보 */
        if (snprintf(tmp, sizeof(tmp), "%s%.*s", docroot, (int)end, path) >= (int)sizeof(tmp)) return 404;
        struct stat st;
        if (stat(tmp, &st) != 0) return 404;
        if (S_ISREG(st.st_mode)) {
            /* realpath로 ".."과 링크를 풀고 cgi-bin 안인지 확인 (경로 탈출 차단) */
            if (realpath(tmp, real) == NULL) return 404;
            size_t cl = strlen(cgidir);
            if (strncmp(real, cgidir, cl) != 0 || real[cl] != '/') return 403;
            if (access(real, X_OK) != 0) return 403;      /* 실행 권한 없음 */
            strcpy(script, real);
            snprintf(script_name, PATH_MAX, "%.*s", (int)end, path);
            snprintf(path_info, PATH_MAX, "%s", path + end);
            return 0;
        }
        if (!S_ISDIR(st.st_mode) || end == plen) return 403;   /* 폴더 자체는 실행 불가 */
        pos = end + 1;
    }
    return 404;
}

/* env_add — "이름=값" 문자열을 만들어 목록에 추가 (fork 전에 준비) */
static void env_add(char **env, int *n, const char *name, const char *value)
{
    if (*n >= CGI_MAX_ENV - 1 || !value) return;
    size_t len = strlen(name) + strlen(value) + 2;
    char *s = malloc(len);
    if (!s) return;
    snprintf(s, len, "%s=%s", name, value);
    env[(*n)++] = s;
    env[*n] = NULL;
}

/* build_env — RFC 3875의 CGI 메타 변수 + 요청 헤더(HTTP_*)를 환경 변수로 만든다 */
static int build_env(char **env, const server_config_t *cfg, const http_request_t *req, const char *peer,
                     const char *script, const char *script_name, const char *path_info)
{
    int n = 0;
    char buf[64], host[256];
    env[0] = NULL;

    env_add(env, &n, "GATEWAY_INTERFACE", "CGI/1.1");
    env_add(env, &n, "SERVER_SOFTWARE", SERVER_NAME);
    env_add(env, &n, "SERVER_PROTOCOL", req->minor ? "HTTP/1.1" : "HTTP/1.0");
    snprintf(buf, sizeof(buf), "%d", cfg->port);
    env_add(env, &n, "SERVER_PORT", buf);
    const char *h = http_get_header(req, "Host");
    snprintf(host, sizeof(host), "%.*s", h ? (int)strcspn(h, ":") : 9, h ? h : "localhost");
    env_add(env, &n, "SERVER_NAME", host);
    env_add(env, &n, "REQUEST_METHOD", req->method_str);
    env_add(env, &n, "REQUEST_URI", req->target);
    env_add(env, &n, "SCRIPT_NAME", script_name);
    env_add(env, &n, "SCRIPT_FILENAME", script);
    env_add(env, &n, "PATH_INFO", path_info);
    env_add(env, &n, "QUERY_STRING", req->query);
    env_add(env, &n, "DOCUMENT_ROOT", cfg->docroot);
    env_add(env, &n, "PATH", "/usr/local/bin:/usr/bin:/bin");   /* 서버의 환경을 통째로 넘기지 않음 */

    /* "127.0.0.1:50312" → REMOTE_ADDR, REMOTE_PORT */
    char ip[64];
    snprintf(ip, sizeof(ip), "%s", peer);
    char *colon = strrchr(ip, ':');
    if (colon) { *colon = '\0'; env_add(env, &n, "REMOTE_PORT", colon + 1); }
    env_add(env, &n, "REMOTE_ADDR", ip);

    if (req->body_len > 0 || req->method == HTTP_POST) {
        snprintf(buf, sizeof(buf), "%zu", req->body_len);
        env_add(env, &n, "CONTENT_LENGTH", buf);
    }
    env_add(env, &n, "CONTENT_TYPE", http_get_header(req, "Content-Type"));

    /* 나머지 헤더: "User-Agent" → HTTP_USER_AGENT */
    for (int i = 0; i < req->nheaders; i++) {
        const char *name = req->headers[i].name;
        if (strcasecmp(name, "Content-Type") == 0 || strcasecmp(name, "Content-Length") == 0) continue;
        /* httpoxy 취약점 차단: "Proxy:" 헤더가 HTTP_PROXY가 되면 스크립트의 HTTP 라이브러리가
         * 공격자 서버를 프록시로 쓰게 됨 → 전달하지 않는다 */
        if (strcasecmp(name, "Proxy") == 0) continue;
        char key[128] = "HTTP_";
        size_t k = 5;
        for (const char *p = name; *p && k < sizeof(key) - 1; p++)
            key[k++] = *p == '-' ? '_' : (char)toupper((unsigned char)*p);
        key[k] = '\0';
        env_add(env, &n, key, req->headers[i].value);
    }
    return n;
}

static void env_free(char **env)
{
    for (int i = 0; env[i]; i++) free(env[i]);
}

/* cgi_parse_headers — 스크립트 출력의 헤더 부분을 해석해 응답 헤더 문자열을 만든다.
 *   결과: 상태 코드, 형식이 틀리면 -1 */
int cgi_parse_headers(char *hdr, char *out, size_t outlen, char *reason, size_t rlen)
{
    int status = 0, has_type = 0, has_loc = 0;
    size_t o = 0;
    out[0] = '\0';
    reason[0] = '\0';
    char *save, *line = strtok_r(hdr, "\n", &save);
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r') line[--l] = '\0';      /* CRLF, LF 둘 다 허용 */
        if (l == 0) continue;
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return -1;
        *colon = '\0';
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        for (char *p = line; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return -1;

        if (strcasecmp(line, "Status") == 0) {               /* "Status: 404 Not Found" → 상태 줄로 */
            char *end;
            long s = strtol(v, &end, 10);
            if (s < 100 || s > 599) return -1;
            status = (int)s;
            while (*end == ' ') end++;
            snprintf(reason, rlen, "%.100s", *end ? end : http_reason(status));
            continue;
        }
        /* 연결/길이 관련 헤더는 서버가 직접 정함 */
        if (strcasecmp(line, "Content-Length") == 0 || strcasecmp(line, "Transfer-Encoding") == 0 ||
            strcasecmp(line, "Connection") == 0)
            continue;
        if (strcasecmp(line, "Content-Type") == 0) has_type = 1;
        if (strcasecmp(line, "Location") == 0) has_loc = 1;
        int k = snprintf(out + o, outlen - o, "%s: %s\r\n", line, v);
        if (k < 0 || (size_t)k >= outlen - o) return -1;
        o += (size_t)k;
    }
    if (!has_type && !has_loc && !status) return -1;        /* 최소한 하나는 있어야 올바른 CGI 출력 */
    if (!status) status = has_loc ? 302 : 200;              /* Location만 있으면 리다이렉트 */
    if (!reason[0]) snprintf(reason, rlen, "%s", http_reason(status));
    return status;
}

/* send_chunk — 클라이언트에 본문 한 조각 전송 (chunked면 "크기\r\n" + 데이터 + "\r\n"을
 *              writev 한 번으로 묶어 보냄: 작은 write 3번보다 시스템 호출·패킷이 적음) */
static int send_chunk(int fd, int chunked, const char *data, size_t n)
{
    if (n == 0) return 0;
    if (!chunked) return net_write_all(fd, data, n);
    char hdr[24];
    int hl = snprintf(hdr, sizeof(hdr), "%zx\r\n", n);
    struct iovec iov[3] = {
        { hdr, (size_t)hl }, { (void *)data, n }, { "\r\n", 2 },
    };
    return net_writev_all(fd, iov, 3);
}

/* reap — 자식을 회수한다. 마감까지 끝나지 않으면 강제 종료. 결과: 종료 상태 (waitpid status) */
static int reap(pid_t pid, time_t deadline)
{
    int st = 0;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) return st;
        if (time(NULL) >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
            return st;
        }
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

int cgi_serve(int fd, const server_config_t *cfg, const http_request_t *req, const char *peer,
              int keep, int *keep_out, long long *sent)
{
    char script[PATH_MAX], script_name[PATH_MAX], path_info[PATH_MAX];
    *sent = 0;
    *keep_out = 0;

    int st = find_script(cfg->docroot, req->path, script, script_name, path_info);
    if (st) return fail(fd, st, NULL, keep_out, sent);

    /* fork 전에 준비: 멀티스레드 프로그램에서 fork한 자식은 exec 전까지 malloc 등을 쓰면 위험 */
    char *env[CGI_MAX_ENV];
    build_env(env, cfg, req, peer, script, script_name, path_info);
    char *argv[] = { script, NULL };
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", script);
    *strrchr(dir, '/') = '\0';
    struct rlimit rl;
    int maxfd = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 65536)
                ? (int)rl.rlim_cur : 65536;

    int in[2], out[2];          /* in: 서버 → 스크립트 표준 입력, out: 스크립트 표준 출력 → 서버 */
    if (pipe(in) != 0) { env_free(env); return fail(fd, 500, "pipe 실패", keep_out, sent); }
    if (pipe(out) != 0) { close(in[0]); close(in[1]); env_free(env); return fail(fd, 500, "pipe 실패", keep_out, sent); }

    pid_t pid = fork();
    if (pid == 0) {
        /* ---- 자식 ---- */
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        /* 0,1,2를 뺀 모든 fd를 닫는다: 클라이언트 소켓·listening 소켓이 스크립트에 새면
         * 스크립트가 백그라운드로 남을 때 연결이 안 닫히는 등 자원이 샌다
         * Linux: close_range 시스템 콜 한 번으로 끝냄 (ulimit이 크면 루프로 수십만 번 close하느라
         *        CGI 한 번에 수십 ms가 걸렸음 → 부하 테스트에서 발견해 개선)
         * 그 밖(macOS 등): 한도까지 하나씩 닫음 (macOS 기본 한도는 256이라 빠름) */
#if defined(__linux__) && defined(SYS_close_range)
        if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
            for (int i = 3; i < maxfd; i++) close(i);
        /* 서버가 바꿔 둔 시그널 설정을 기본으로 되돌림 (워커는 SIGINT를 막아 둔 상태였음) */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        if (chdir(dir) != 0) _exit(126);       /* CGI 관례: 스크립트가 있는 폴더에서 실행 */
        execve(script, argv, env);
        _exit(127);                            /* exec 실패 (예: #! 해석기 없음) */
    }
    env_free(env);
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]); close(out[0]);
        return fail(fd, 503, "fork 실패", keep_out, sent);
    }

    /* ---- 부모: 본문 쓰기와 출력 읽기를 poll로 동시에 ---- */
    net_set_nonblock(in[1], 1);
    net_set_nonblock(out[0], 1);
    time_t deadline = time(NULL) + CGI_TIMEOUT_S;
    size_t written = 0;
    int in_open = 1;
    if (req->body_len == 0) { close(in[1]); in_open = 0; }   /* 본문이 없으면 바로 EOF를 줌 */

    static _Thread_local char hbuf[CGI_MAX_HEADER + 1];
    static _Thread_local char rbuf[65536];
    size_t hlen = 0;
    int headers_done = 0, chunked = 0, status = 0, rc = 0, timed_out = 0;
    int head_only = req->method == HTTP_HEAD;

    for (;;) {
        struct pollfd p[2];
        int np = 0;
        p[np].fd = out[0]; p[np].events = POLLIN; np++;
        if (in_open) { p[np].fd = in[1]; p[np].events = POLLOUT; np++; }
        int left = (int)(deadline - time(NULL));
        if (left <= 0) { timed_out = 1; break; }
        int n = poll(p, (nfds_t)np, left * 1000);
        if (n < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (n == 0) { timed_out = 1; break; }

        /* 본문 쓰기: 스크립트가 읽는 만큼만 (파이프 버퍼가 차면 다음 차례에) */
        if (in_open && (p[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(in[1], req->body + written, req->body_len - written);
            if (w > 0) written += (size_t)w;
            if ((w < 0 && errno != EAGAIN && errno != EINTR) || written == req->body_len) {
                close(in[1]);                  /* 다 썼거나 스크립트가 안 읽고 닫음 → EOF 전달 */
                in_open = 0;
            }
        }
        if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;

        /* 출력 읽기 */
        char *dst = headers_done ? rbuf : hbuf + hlen;
        size_t cap = headers_done ? sizeof(rbuf) : CGI_MAX_HEADER - hlen;
        ssize_t r = read(out[0], dst, cap);
        if (r < 0) { if (errno == EAGAIN || errno == EINTR) continue; rc = -1; break; }
        if (r == 0) break;                     /* 스크립트가 출력을 닫음 = 끝 */

        if (headers_done) {
            if (!head_only && send_chunk(fd, chunked, rbuf, (size_t)r) != 0) { rc = -1; break; }
            if (!head_only) *sent += r;
            continue;
        }

        /* 아직 헤더 부분: 빈 줄("\n\n" 또는 "\r\n\r\n")이 나올 때까지 모은다 */
        hlen += (size_t)r;
        hbuf[hlen] = '\0';
        char *sep = strstr(hbuf, "\r\n\r\n");
        size_t seplen = 4;
        char *sep2 = strstr(hbuf, "\n\n");
        if (!sep || (sep2 && sep2 < sep)) { sep = sep2; seplen = 2; }
        if (!sep) {
            if (hlen >= CGI_MAX_HEADER) { rc = 502; break; }
            continue;
        }
        *sep = '\0';
        char *body = sep + seplen;
        size_t blen = hlen - (size_t)(body - hbuf);

        char extra[CGI_MAX_HEADER], reason[128];
        status = cgi_parse_headers(hbuf, extra, sizeof(extra), reason, sizeof(reason));
        if (status < 0) { rc = 502; break; }

        /* 204/304/1xx는 본문이 없어야 하는 응답 → HEAD처럼 본문을 버림 (스크립트가 실수로 써도) */
        if (status == 204 || status == 304 || status < 200) head_only = 1;
        /* 응답 끝 표시: HTTP/1.1이면 chunked로 연결 유지, 1.0이면 연결을 닫아 끝을 알림 */
        chunked = req->minor == 1 && !head_only;
        int k = keep && (chunked || head_only);
        char head[CGI_MAX_HEADER + 512], date[64];
        http_date(time(NULL), date, sizeof(date));
        int hl = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: " SERVER_NAME "\r\n%s%s%s",
                          status, reason, date, extra,
                          chunked ? "Transfer-Encoding: chunked\r\n" : "",
                          k ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
        if (hl < 0 || hl >= (int)sizeof(head) || net_write_all(fd, head, (size_t)hl) != 0) { rc = -1; break; }
        headers_done = 1;
        *keep_out = k;
        if (!head_only) {
            if (send_chunk(fd, chunked, body, blen) != 0) { rc = -1; break; }
            *sent += (long long)blen;
        }
    }

    if (in_open) close(in[1]);
    close(out[0]);
    int wst = reap(pid, timed_out ? 0 : time(NULL) + 2);   /* 시간 초과면 즉시 강제 종료 */

    if (timed_out) {
        fprintf(stderr, "[CGI] %s: %d초 시간 초과로 강제 종료\n", script_name, CGI_TIMEOUT_S);
        if (!headers_done) return fail(fd, 504, NULL, keep_out, sent);
        *keep_out = 0;
        return -1;                              /* 이미 보내는 중이었으면 끊는 수밖에 없음 */
    }
    if (rc == -1) { *keep_out = 0; return -1; }
    if (!headers_done) {                        /* 헤더 없이 끝났거나 형식이 틀림 */
        char why[256];
        snprintf(why, sizeof(why), "%.100s: 올바른 CGI 헤더를 출력하지 않음 (종료 코드 %d)", script_name,
                 WIFEXITED(wst) ? WEXITSTATUS(wst) : -1);
        return fail(fd, 502, why, keep_out, sent);
    }
    if (chunked && net_write_all(fd, "0\r\n\r\n", 5) != 0) { *keep_out = 0; return -1; }  /* 마지막 조각 */
    return status;
}
