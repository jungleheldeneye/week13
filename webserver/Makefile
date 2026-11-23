CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
THREAD_FLAGS = -pthread

TARGET = webserver
HTTP_SOURCES = src/http_reader.c src/http_parser.c src/http_response.c
CORE_SOURCES = $(HTTP_SOURCES) src/config.c src/router.c src/static_file.c src/thread_pool.c
SOURCES = src/main.c src/server.c $(CORE_SOURCES)
OBJECTS = $(SOURCES:.c=.o)
TEST_TARGETS = tests/test_http tests/test_router tests/test_config tests/test_thread_pool
TEST_OBJECTS = $(TEST_TARGETS:=.o)
SANITIZER_TARGETS = $(TEST_TARGETS:=_asan)
SANITIZER_SERVER = webserver_asan
DEPFILES = $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

.PHONY: all test integration-test sanitize-test sanitize-integration-test clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -MMD -MP -c $< -o $@

tests/%.o: tests/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c $< -o $@

tests/test_http: tests/test_http.o $(HTTP_SOURCES:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread -o $@ $^ $(LDLIBS)

tests/test_router: tests/test_router.o $(CORE_SOURCES:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread -o $@ $^ $(LDLIBS)

tests/test_config: tests/test_config.o src/config.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_thread_pool: tests/test_thread_pool.o src/thread_pool.o
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test: $(TEST_TARGETS)
	./tests/test_http
	./tests/test_router
	./tests/test_config
	./tests/test_thread_pool

integration-test: $(TARGET)
	python3 tests/test_server.py ./$(TARGET)

tests/test_%_asan: tests/test_%.c $(CORE_SOURCES) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -fsanitize=address,undefined \
		-fno-omit-frame-pointer -o $@ $< $(CORE_SOURCES) $(LDLIBS)

sanitize-test: $(SANITIZER_TARGETS)
	./tests/test_http_asan
	./tests/test_router_asan
	./tests/test_config_asan
	./tests/test_thread_pool_asan

$(SANITIZER_SERVER): $(SOURCES) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) $(LDFLAGS) -fsanitize=address,undefined \
		-fno-omit-frame-pointer -o $@ $(SOURCES) $(LDLIBS)

sanitize-integration-test: $(SANITIZER_SERVER)
	python3 tests/test_server.py ./$(SANITIZER_SERVER)

clean:
	rm -f $(TARGET) $(SANITIZER_SERVER) $(OBJECTS) $(TEST_TARGETS) $(SANITIZER_TARGETS) $(TEST_OBJECTS) $(DEPFILES)
	rm -rf $(SANITIZER_TARGETS:=.dSYM) $(SANITIZER_SERVER).dSYM

-include $(DEPFILES)
