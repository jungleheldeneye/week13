/*
 * cache.h — Forward Proxy 응답 캐시 (메모리, LRU)
 *
 * 이 파일이 하는 일:
 *   Forward Proxy로 받아 온 응답을 메모리에 잠시 보관했다가, 같은 URL 요청이 다시 오면
 *   원 서버에 가지 않고 바로 돌려준다. (응답 시간 단축 + 원 서버 부하 감소)
 *
 * 규칙 (RFC 9111을 단순화):
 *   - 저장: GET 요청의 200 응답, 길이(Content-Length)를 아는 본문, 1MB 이하만
 *   - 저장 안 함: Cache-Control에 no-store / private / no-cache, Set-Cookie 있음, Vary 있음,
 *                요청에 Authorization 있음 (사용자별 응답일 수 있음)
 *   - 유효 시간: s-maxage > max-age > 기본 60초 (max-age=0이면 저장 안 함)
 *   - 찾기: GET/HEAD만. 요청에 Cache-Control: no-cache 또는 Pragma: no-cache면 캐시를 건너뛰고 새로 받음
 *   - 용량: 전체 32MB / 1024개 넘으면 가장 오래 안 쓴 것부터 버림 (LRU)
 *   - 동시성: 뮤텍스 하나로 보호. 찾은 응답은 복사본을 넘겨서, 느린 클라이언트에 보내는 동안 잠그지 않음
 *   - fork 모드에서는 연결마다 프로세스가 따로라 캐시가 공유되지 않음 (pool/thread 모드에서 효과)
 */
#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>
#include "http.h"

#define CACHE_MAX_OBJECT   (1024 * 1024)        /* 응답 하나 최대 크기 */
#define CACHE_MAX_TOTAL    (32 * 1024 * 1024)   /* 전체 용량 */
#define CACHE_MAX_ENTRIES  1024
#define CACHE_DEFAULT_TTL  60                   /* 유효 시간 지정이 없을 때(초) */

/* 찾은 응답 (복사본: 다 쓰면 cache_hit_free) */
typedef struct {
    char  *head;        /* 상태 줄 + 헤더 (Connection 줄과 끝 빈 줄은 빠져 있음) */
    size_t head_len;
    char  *body;
    size_t body_len;
    long   age;         /* 저장된 지 몇 초 (Age 헤더로 알려 줌) */
} cache_hit_t;

/* cache_request_ok — 이 요청을 캐시에서 찾아도 되는지 (GET/HEAD, no-cache 아님, Authorization 없음) */
int cache_request_ok(const http_request_t *req);

/*
 * cache_ttl — 응답 헤더를 보고 저장할 시간(초)을 정한다.
 *   입력: Cache-Control 값(없으면 NULL), Set-Cookie/Vary 헤더가 있는지
 *   결과: 저장할 초, 저장하면 안 되면 -1
 */
int cache_ttl(const char *cache_control, int has_set_cookie, int has_vary);

/* cache_get — key(URL)로 찾기. 결과: 1 찾음(out 채움), 0 없음/만료 */
int cache_get(const char *key, cache_hit_t *out);
void cache_hit_free(cache_hit_t *h);

/* cache_put — 저장 (같은 key가 있으면 교체, 공간이 모자라면 오래된 것부터 버림) */
void cache_put(const char *key, const char *head, size_t head_len, const char *body, size_t body_len, int ttl);

/* cache_stats — 적중/실패 횟수, 저장된 개수와 바이트 (종료 시 출력용) */
void cache_stats(long *hits, long *misses, int *entries, size_t *bytes);

/* cache_clear — 전부 비움 (종료 시 메모리 반환, 테스트용) */
void cache_clear(void);

#endif
