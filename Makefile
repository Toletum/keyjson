CC ?= cc
BUILD_DIR ?= build
CFLAGS ?= -O3
CPPFLAGS += -Iinclude -Ithird_party/yyjson
STRICT_CFLAGS = -std=c11 -Wall -Wextra -Werror

APP = $(BUILD_DIR)/keyjson
TEST_KV = $(BUILD_DIR)/test-kv
TEST_HTTP = $(BUILD_DIR)/test-http
TEST_HTTP_PORT ?= 8080
BENCH_KV = $(BUILD_DIR)/bench-kv
BENCH_HTTP = $(BUILD_DIR)/bench-http
CORE_OBJECTS = $(BUILD_DIR)/kv.o $(BUILD_DIR)/http.o $(BUILD_DIR)/yyjson.o

.PHONY: all test test-http sanitize bench-kv bench-http clean container run

all: $(APP)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/main.o: src/main.c include/http.h include/kv.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(STRICT_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/kv.o: src/kv.c include/kv.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(STRICT_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/http.o: src/http.c include/http.h include/kv.h third_party/yyjson/yyjson.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(STRICT_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/yyjson.o: third_party/yyjson/yyjson.c third_party/yyjson/yyjson.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(STRICT_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(APP): $(BUILD_DIR)/main.o $(CORE_OBJECTS)
	$(CC) $(STRICT_CFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(TEST_KV): tests/test_kv.c $(BUILD_DIR)/kv.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(STRICT_CFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

test: $(TEST_KV)
	./$(TEST_KV)

$(TEST_HTTP): tests/test_http_robustness.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(STRICT_CFLAGS) $(CFLAGS) $< $(LDFLAGS) -pthread -o $@

test-http: $(TEST_HTTP)
	./$(TEST_HTTP) $(TEST_HTTP_PORT)

$(BENCH_KV): tests/bench_kv.c $(BUILD_DIR)/kv.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(STRICT_CFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BENCH_HTTP): tests/bench_http.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(STRICT_CFLAGS) $(CFLAGS) $< $(LDFLAGS) -pthread -o $@

bench-kv: $(BENCH_KV)
	./$(BENCH_KV)

bench-http: $(BENCH_HTTP)
	./$(BENCH_HTTP)

sanitize:
	$(MAKE) BUILD_DIR=build/sanitize CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" all test build/sanitize/test-http

container:
	podman build -t localhost/keyjson:dev -f Containerfile .

run: container
	podman run --rm --name keyjson -p 127.0.0.1:8080:8080 localhost/keyjson:dev

clean:
	rm -rf build

-include $(wildcard $(BUILD_DIR)/*.d)
