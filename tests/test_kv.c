#define _POSIX_C_SOURCE 200809L

#include "kv.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEST_KEY_COUNT 2048U

static bool count_key(const char *key, size_t key_len, void *context) {
    size_t *count = context;
    assert(key != NULL);
    assert(key_len > 0);
    (*count)++;
    return true;
}

static void make_pair(size_t index,
                      char *key,
                      size_t key_capacity,
                      char *value,
                      size_t value_capacity) {
    int key_length = snprintf(key, key_capacity, "key-%04zu", index);
    int value_length = snprintf(value, value_capacity, "value-%04zu", index);
    assert(key_length > 0 && (size_t)key_length < key_capacity);
    assert(value_length > 0 && (size_t)value_length < value_capacity);
}

static void test_ttl(void) {
    kv_store *store = kv_store_create();
    const char *value = NULL;
    size_t value_len = 0U;
    bool created = false;
    kv_stats stats;
    size_t visited = 0U;
    struct timespec delay = {.tv_sec = 1, .tv_nsec = 100000000L};

    assert(store != NULL);
    assert(kv_store_set_with_ttl(store, "expires", 7U, "1", 1U, 1U,
                                 &created) == KV_STATUS_OK);
    assert(created);
    assert(kv_store_set_with_ttl(store, "cleared", 7U, "2", 1U, 1U,
                                 &created) == KV_STATUS_OK);
    assert(created);
    assert(kv_store_set(store, "cleared", 7U, "3", 1U, &created) ==
           KV_STATUS_OK);
    assert(!created);
    assert(kv_store_set_with_ttl(store, "later", 5U, "4", 1U, 3U,
                                 &created) == KV_STATUS_OK);
    assert(created);
    assert(kv_store_set_with_ttl(store, "overflow", 8U, "1", 1U,
                                 UINT64_MAX, NULL) == KV_STATUS_INVALID_ARGUMENT);

    while (nanosleep(&delay, &delay) != 0) {
        assert(errno == EINTR);
    }

    kv_store_get_stats(store, &stats);
    assert(stats.key_count == 2U);
    assert(stats.stored_bytes > 0U);
    assert(stats.delete_count == 0U);
    assert(kv_store_get(store, "expires", 7U, &value, &value_len) ==
           KV_STATUS_NOT_FOUND);
    assert(kv_store_get(store, "cleared", 7U, &value, &value_len) ==
           KV_STATUS_OK);
    assert(value_len == 1U && value[0] == '3');
    assert(kv_store_get(store, "later", 5U, &value, &value_len) ==
           KV_STATUS_OK);
    assert(value_len == 1U && value[0] == '4');
    assert(kv_store_visit(store, count_key, &visited));
    assert(visited == 2U);

    kv_store_destroy(store);
}

int main(void) {
    kv_store *store = kv_store_create();
    assert(store != NULL);

    const char *value = NULL;
    size_t value_len = 0;
    assert(kv_store_get(store, "missing", 7, &value, &value_len) ==
           KV_STATUS_NOT_FOUND);
    assert(kv_store_set(store, "", 0, "1", 1, NULL) ==
           KV_STATUS_INVALID_ARGUMENT);

    bool created = false;
    assert(kv_store_set(store, "alpha", 5, "1", 1, &created) == KV_STATUS_OK);
    assert(created);
    assert(kv_store_get(store, "alpha", 5, &value, &value_len) == KV_STATUS_OK);
    assert(value_len == 1 && memcmp(value, "1", value_len) == 0);
    assert(kv_store_set(store, "alpha", 5, "{\"n\":2}", 7, &created) ==
           KV_STATUS_OK);
    assert(!created);
    assert(kv_store_get(store, "alpha", 5, &value, &value_len) == KV_STATUS_OK);
    assert(value_len == 7 && memcmp(value, "{\"n\":2}", value_len) == 0);
    assert(kv_store_delete(store, "alpha", 5) == KV_STATUS_OK);
    assert(kv_store_delete(store, "alpha", 5) == KV_STATUS_NOT_FOUND);
    assert(kv_store_get(store, "alpha", 5, &value, &value_len) ==
           KV_STATUS_NOT_FOUND);

    for (size_t i = 0; i < TEST_KEY_COUNT; i++) {
        char key[32];
        char item_value[32];
        make_pair(i, key, sizeof(key), item_value, sizeof(item_value));
        assert(kv_store_set(store, key, strlen(key), item_value,
                            strlen(item_value), &created) == KV_STATUS_OK);
        assert(created);
    }

    for (size_t i = 0; i < TEST_KEY_COUNT; i++) {
        char key[32];
        char expected[32];
        make_pair(i, key, sizeof(key), expected, sizeof(expected));
        assert(kv_store_get(store, key, strlen(key), &value, &value_len) ==
               KV_STATUS_OK);
        assert(value_len == strlen(expected));
        assert(memcmp(value, expected, value_len) == 0);
    }

    for (size_t i = 0; i < TEST_KEY_COUNT; i += 2U) {
        char key[32];
        char expected[32];
        make_pair(i, key, sizeof(key), expected, sizeof(expected));
        assert(kv_store_delete(store, key, strlen(key)) == KV_STATUS_OK);
    }

    for (size_t i = 0; i < TEST_KEY_COUNT; i += 2U) {
        char key[32];
        char item_value[32];
        int key_length = snprintf(key, sizeof(key), "replacement-%04zu", i);
        int value_length = snprintf(item_value, sizeof(item_value), "new-%04zu", i);
        assert(key_length > 0 && (size_t)key_length < sizeof(key));
        assert(value_length > 0 && (size_t)value_length < sizeof(item_value));
        assert(kv_store_set(store, key, (size_t)key_length, item_value,
                            (size_t)value_length, &created) == KV_STATUS_OK);
        assert(created);
    }

    for (size_t i = 1; i < TEST_KEY_COUNT; i += 2U) {
        char key[32];
        char expected[32];
        make_pair(i, key, sizeof(key), expected, sizeof(expected));
        assert(kv_store_get(store, key, strlen(key), &value, &value_len) ==
               KV_STATUS_OK);
        assert(value_len == strlen(expected));
        assert(memcmp(value, expected, value_len) == 0);
    }

    kv_stats stats;
    kv_store_get_stats(store, &stats);
    assert(stats.key_count == TEST_KEY_COUNT);
    assert(stats.capacity >= TEST_KEY_COUNT);
    assert(stats.stored_bytes > 0);
    assert(stats.set_count == 1U + 1U + TEST_KEY_COUNT + TEST_KEY_COUNT / 2U);
    assert(stats.delete_count == 1U + TEST_KEY_COUNT / 2U);

    size_t visited = 0;
    assert(kv_store_visit(store, count_key, &visited));
    assert(visited == stats.key_count);

    kv_store_destroy(store);

    store = kv_store_create();
    assert(store != NULL);
    for (size_t i = 0; i < 11U; i++) {
        char key[16];
        int key_length = snprintf(key, sizeof(key), "fill-%zu", i);
        assert(key_length > 0 && (size_t)key_length < sizeof(key));
        assert(kv_store_set(store, key, (size_t)key_length, "1", 1, NULL) ==
               KV_STATUS_OK);
    }
    assert(kv_store_delete(store, "fill-0", 6) == KV_STATUS_OK);
    assert(kv_store_set(store, "fill-new", 8, "1", 1, NULL) == KV_STATUS_OK);
    kv_store_get_stats(store, &stats);
    assert(stats.capacity == 16U);
    kv_store_destroy(store);

    test_ttl();

    puts("kv tests passed");
    return 0;
}
