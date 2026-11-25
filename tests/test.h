/*
 * test.h — 외부 라이브러리 없는 아주 작은 테스트 도구
 *
 *   CHECK(조건)       : 조건이 거짓이면 파일:줄과 조건식을 출력하고 실패로 센다
 *   CHECK_STR(a, b)   : 두 문자열이 같은지 검사
 *   RUN(함수)         : 테스트 묶음 하나를 이름과 함께 실행
 */
#ifndef TEST_H
#define TEST_H
#include <stdio.h>
#include <string.h>

extern int t_run, t_fail;

#define CHECK(cond) do { t_run++; if (!(cond)) { t_fail++; \
    printf("  실패 %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)
#define RUN(fn) do { printf("- %s\n", #fn); fn(); } while (0)

#endif
