/*
 * cache.c — Forward Proxy 응답 캐시 구현 (cache.h 설명 참고)
 *
 * 자료 구조:
 *   - 해시 테이블(256칸, 체이닝): key로 빠르게 찾기
 *   - 이중 연결 리스트: 맨 앞 = 가장 최근에 쓴 것, 맨 뒤 = 가장 오래 안 쓴 것 (LRU 제거 대상)
 *   항목 하나가 두 구조에 동시에 들어 있다.
 */
#include "cache.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define NBUCKET 256

typedef struct entry {
    char *key;
    unsigned hash;
    char *head;  size_t head_len;
    char *body;  size_t body_len;
    time_t stored, expires;
    struct entry *hnext;            /* 같은 해시 칸의 다음 항목 */
    struct entry *prev, *next;      /* LRU 리스트 */
} entry_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static entry_t *g_bucket[NBUCKET];
static entry_t *g_head, *g_tail;    /* LRU: head = 최근, tail = 오래됨 */
static int g_count;
static size_t g_bytes;
static long g_hits, g_misses;

/* FNV-1a 해시: 짧은 문자열에 빠르고 고르게 퍼짐 */
static unsigned hash_str(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static void lru_unlink(entry_t *e)
{
    if (e->prev) e->prev->next = e->next; else g_head = e->next;
    if (e->next) e->next->prev = e->prev; else g_tail = e->prev;
    e->prev = e->next = NULL;
}

static void lru_push_front(entry_t *e)
{
    e->prev = NULL;
    e->next = g_head;
    if (g_head) g_head->prev = e;
    g_head = e;
    if (!g_tail) g_tail = e;
}

/* remove_entry — 해시 칸과 LRU 리스트에서 빼고 메모리 반환 (잠금 상태에서 호출) */
static void remove_entry(entry_t *e)
{
    entry_t **pp = &g_bucket[e->hash % NBUCKET];
    while (*pp && *pp != e) pp = &(*pp)->hnext;
    if (*pp) *pp = e->hnext;
    lru_unlink(e);
    g_count--;
    g_bytes -= e->head_len + e->body_len;
    free(e->key); free(e->head); free(e->body); free(e);
}

static entry_t *find(const char *key, unsigned h)
{
    for (entry_t *e = g_bucket[h % NBUCKET]; e; e = e->hnext)
        if (e->hash == h && strcmp(e->key, key) == 0) return e;
    return NULL;
}

int cache_request_ok(const http_request_t *req)
{
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) return 0;
    if (http_get_header(req, "Authorization")) return 0;
    const char *cc = http_get_header(req, "Cache-Control");
    const char *pragma = http_get_header(req, "Pragma");
    if (cc && (http_has_token(cc, "no-cache") || http_has_token(cc, "no-store"))) return 0;
    if (pragma && http_has_token(pragma, "no-cache")) return 0;   /* HTTP/1.0 방식의 "새로 받아" */
    return 1;
}

/* directive_num — "max-age=120, public" 에서 이름=숫자 값 찾기. 없으면 -1 */
static long directive_num(const char *cc, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = cc; *p; ) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == '=') {
            const char *v = p + nl + 1;
            if (*v == '"') v++;
            if (!isdigit((unsigned char)*v)) return -1;
            long n = 0;
            while (isdigit((unsigned char)*v) && n < 100000000) n = n * 10 + (*v++ - '0');
            return n;
        }
        p += strcspn(p, ",");
    }
    return -1;
}

int cache_ttl(const char *cc, int has_set_cookie, int has_vary)
{
    if (has_set_cookie || has_vary) return -1;     /* 사용자별/요청 조건별 응답 → 공유 캐시에 두면 안 됨 */
    if (!cc) return CACHE_DEFAULT_TTL;
    if (http_has_token(cc, "no-store") || http_has_token(cc, "private") || http_has_token(cc, "no-cache"))
        return -1;
    long t = directive_num(cc, "s-maxage");         /* 공유 캐시(프록시) 전용 값이 우선 */
    if (t < 0) t = directive_num(cc, "max-age");
    if (t < 0) return CACHE_DEFAULT_TTL;
    if (t == 0) return -1;
    return t > 86400 ? 86400 : (int)t;             /* 너무 길면 하루로 제한 */
}

int cache_get(const char *key, cache_hit_t *out)
{
    unsigned h = hash_str(key);
    time_t now = time(NULL);
    int found = 0;
    pthread_mutex_lock(&g_mu);
    entry_t *e = find(key, h);
    if (e && e->expires <= now) { remove_entry(e); e = NULL; }   /* 만료 → 버리고 실패 처리 */
    if (e) {
        /* 복사본을 만들어 넘김: 잠금을 풀고 보내는 동안 다른 스레드가 이 항목을 지워도 안전 */
        out->head = malloc(e->head_len);
        out->body = malloc(e->body_len ? e->body_len : 1);
        if (out->head && out->body) {
            memcpy(out->head, e->head, e->head_len);
            memcpy(out->body, e->body, e->body_len);
            out->head_len = e->head_len;
            out->body_len = e->body_len;
            out->age = (long)(now - e->stored);
            lru_unlink(e);
            lru_push_front(e);                     /* 방금 썼으니 "최근" 쪽으로 */
            found = 1;
        } else {
            free(out->head); free(out->body);
        }
    }
    if (found) g_hits++; else g_misses++;
    pthread_mutex_unlock(&g_mu);
    return found;
}

void cache_hit_free(cache_hit_t *h)
{
    free(h->head);
    free(h->body);
    h->head = h->body = NULL;
}

void cache_put(const char *key, const char *head, size_t head_len, const char *body, size_t body_len, int ttl)
{
    size_t need = head_len + body_len;
    if (ttl <= 0 || body_len > CACHE_MAX_OBJECT || need > CACHE_MAX_TOTAL) return;
    entry_t *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->key = strdup(key);
    n->head = malloc(head_len);
    n->body = malloc(body_len ? body_len : 1);
    if (!n->key || !n->head || !n->body) { free(n->key); free(n->head); free(n->body); free(n); return; }
    memcpy(n->head, head, head_len);
    memcpy(n->body, body, body_len);
    n->head_len = head_len;
    n->body_len = body_len;
    n->hash = hash_str(key);
    n->stored = time(NULL);
    n->expires = n->stored + ttl;

    pthread_mutex_lock(&g_mu);
    entry_t *old = find(key, n->hash);
    if (old) remove_entry(old);                    /* 같은 URL은 새 응답으로 교체 */
    while (g_tail && (g_bytes + need > CACHE_MAX_TOTAL || g_count >= CACHE_MAX_ENTRIES))
        remove_entry(g_tail);                      /* 공간 확보: 가장 오래 안 쓴 것부터 */
    n->hnext = g_bucket[n->hash % NBUCKET];
    g_bucket[n->hash % NBUCKET] = n;
    lru_push_front(n);
    g_count++;
    g_bytes += need;
    pthread_mutex_unlock(&g_mu);
}

void cache_stats(long *hits, long *misses, int *entries, size_t *bytes)
{
    pthread_mutex_lock(&g_mu);
    *hits = g_hits; *misses = g_misses; *entries = g_count; *bytes = g_bytes;
    pthread_mutex_unlock(&g_mu);
}

void cache_clear(void)
{
    pthread_mutex_lock(&g_mu);
    while (g_tail) remove_entry(g_tail);
    g_hits = g_misses = 0;
    pthread_mutex_unlock(&g_mu);
}
