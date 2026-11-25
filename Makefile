# C 웹 서버 빌드 (macOS / Linux 공용)
CC      ?= cc
CFLAGS  ?= -Wall -Wextra -Werror -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -g -O2
LDFLAGS ?=
LDLIBS  ?= -lpthread

SRC      := $(wildcard src/*.c)
LIB_SRC  := $(filter-out src/main.c, $(SRC))   # main을 뺀 모듈들 = 테스트 대상
TEST_SRC := $(wildcard tests/*.c)

# macOS는 LeakSanitizer를 지원하지 않아 address만 사용
UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
  SAN := -fsanitize=address,undefined
  CFLAGS += -D_DARWIN_C_SOURCE
else
  SAN := -fsanitize=address,leak,undefined
endif

.PHONY: all test cgitest proxytest edgetest check asan tsan clean

all: server

server: $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

test: $(LIB_SRC) $(TEST_SRC)
	$(CC) $(CFLAGS) -o test_runner $(LIB_SRC) $(TEST_SRC) $(LDLIBS)
	./test_runner

asan: $(SRC)
	$(CC) $(CFLAGS) -O0 $(SAN) -fno-omit-frame-pointer -o server_asan $(SRC) $(LDLIBS)

tsan: $(SRC)
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o server_tsan $(SRC) $(LDLIBS)

clean:
	rm -f server server_asan server_tsan test_runner bench

# CGI 통합 테스트 (서버를 직접 띄워 curl로 검사, 약 10초)
cgitest: server
	bash tests/cgi_test.sh

# 예외 상황 테스트 (깨진 요청, 큰 요청, 백엔드 장애, 갑작스러운 연결 종료) — python3 필요, 약 15초
#   메모리 검사까지: make asan && SERVER=./server_asan python3 tests/edge_test.py
edgetest: server
	python3 tests/edge_test.py

# 부하 테스트 도구 (ab 대용, HTTP/1.1 keep-alive + chunked 지원). 표 만들기: bash tests/bench.sh
bench: tools/bench.c
	$(CC) -Wall -Wextra -O2 -o bench tools/bench.c -lpthread

# 프록시 통합 테스트 (Round-Robin, 장애 전환, 502, Forward 캐시, 루프 508)
proxytest: server
	bash tests/proxy_test.sh

# 전체 검증: 단위 테스트 + CGI/프록시/예외 상황 통합 테스트
check: test cgitest proxytest edgetest
