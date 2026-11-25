/*
 * pool.h — 스레드 풀 + poll 대기실 (기본 동시 처리 방식, -m pool)
 *
 * 구조:
 *   [메인 스레드 = 문지기]  poll()로 listening 소켓과 "대기실"의 연결들을 지켜본다.
 *      - 새 연결: accept해서 대기실에 넣는다 (아직 워커를 배정하지 않음)
 *      - 대기실 연결에 데이터 도착: 작업 대기열에 넣는다 → 워커가 처리
 *      - 오래 조용한 연결: 시간 초과로 닫는다 (첫 요청 10초, keep-alive 5초)
 *   [워커 N개]  대기열에서 연결을 꺼내 요청을 처리하고,
 *      keep-alive면 대기실로 돌려보낸다 (다음 요청을 기다리며 워커를 붙잡지 않음)
 *
 *   효과: 요청을 보내지 않는 연결이 워커를 차지하지 않는다 → 워커 8개로 대기 연결 수천 개를 버팀
 */
#ifndef POOL_H
#define POOL_H

#include "config.h"

#define POOL_MAX_CONNS 4096   /* 대기실 정원: 넘으면 새 연결에 503 */
#define POOL_QUEUE_MAX 1024   /* 작업 대기열 길이: 넘으면 503 */

/*
 * pool_run — 스레드 풀 방식으로 서버를 돌린다. 종료 신호가 오면 정리하고 돌아온다.
 *   입력: lfd(listening 소켓), cfg   결과: 0 정상 종료, -1 시작 실패
 */
int pool_run(int lfd, const server_config_t *cfg);

#endif
