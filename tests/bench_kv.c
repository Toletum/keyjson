#define _POSIX_C_SOURCE 200809L

#include "kv.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ENTRIES 100000U
#define DEFAULT_OPERATIONS 1000000U
#define KEY_BUFFER_SIZE 32U

static bool parse_size(const char *text, size_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || *text == '-' || *text == '+') {
        return false;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
        parsed > SIZE_MAX) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static bool monotonic_nanoseconds(uint64_t *value) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }
    *value = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
             (uint64_t)now.tv_nsec;
    return true;
}

static void print_result(const char *name,
                         size_t operations,
                         uint64_t elapsed_ns) {
    double seconds = (double)elapsed_ns / 1000000000.0;
    double operations_per_second = seconds > 0.0
                                      ? (double)operations / seconds
                                      : 0.0;
    printf("%-10s %10zu ops  %8.3f s  %12.0f ops/s\n", name, operations,
           seconds, operations_per_second);
}

int main(int argc, char **argv) {
    size_t entries = DEFAULT_ENTRIES;
    size_t operations = DEFAULT_OPERATIONS;
    char (*keys)[KEY_BUFFER_SIZE] = NULL;
    kv_store *store = NULL;
    const char *stored_value = NULL;
    size_t stored_value_len = 0;
    uint64_t checksum = 0;
    uint64_t start;
    uint64_t end;
    size_t entry_value_len = sizeof("{\"value\":1}") - 1U;
    size_t update_value_len = sizeof("{\"value\":2}") - 1U;
    int result = 1;

    if (argc > 3 || (argc >= 2 && !parse_size(argv[1], &entries)) ||
        (argc >= 3 && !parse_size(argv[2], &operations)) ||
        entries > SIZE_MAX / sizeof(*keys)) {
        fprintf(stderr, "Usage: %s [entries>0] [operations>0]\n", argv[0]);
        return 2;
    }

    keys = calloc(entries, sizeof(*keys));
    store = kv_store_create();
    if (keys == NULL || store == NULL) {
        fprintf(stderr, "Unable to allocate benchmark data\n");
        goto cleanup;
    }
    for (size_t i = 0; i < entries; i++) {
        int length = snprintf(keys[i], sizeof(keys[i]), "bench-%zu", i);
        if (length < 0 || (size_t)length >= sizeof(keys[i])) {
            fprintf(stderr, "Unable to format benchmark key\n");
            goto cleanup;
        }
    }

    printf("KV benchmark: entries=%zu operations_per_phase=%zu\n", entries,
           operations);
    if (!monotonic_nanoseconds(&start)) {
        perror("clock_gettime");
        goto cleanup;
    }
    for (size_t i = 0; i < entries; i++) {
        bool created = false;
        kv_status status = kv_store_set(store, keys[i], strlen(keys[i]),
                                        "{\"value\":1}", entry_value_len,
                                        &created);
        if (status != KV_STATUS_OK || !created) {
            fprintf(stderr, "Insert phase failed at entry %zu\n", i);
            goto cleanup;
        }
    }
    if (!monotonic_nanoseconds(&end)) {
        perror("clock_gettime");
        goto cleanup;
    }
    print_result("insert", entries, end - start);

    if (!monotonic_nanoseconds(&start)) {
        perror("clock_gettime");
        goto cleanup;
    }
    for (size_t i = 0; i < operations; i++) {
        size_t index = i % entries;
        kv_status status = kv_store_get(store, keys[index], strlen(keys[index]),
                                        &stored_value, &stored_value_len);
        if (status != KV_STATUS_OK || stored_value_len != entry_value_len) {
            fprintf(stderr, "GET hit phase failed at operation %zu\n", i);
            goto cleanup;
        }
        checksum += (uint64_t)(unsigned char)stored_value[0];
    }
    if (!monotonic_nanoseconds(&end)) {
        perror("clock_gettime");
        goto cleanup;
    }
    print_result("get-hit", operations, end - start);

    if (!monotonic_nanoseconds(&start)) {
        perror("clock_gettime");
        goto cleanup;
    }
    for (size_t i = 0; i < operations; i++) {
        kv_status status = kv_store_get(store, "missing-benchmark-key", 21,
                                        &stored_value, &stored_value_len);
        if (status != KV_STATUS_NOT_FOUND) {
            fprintf(stderr, "GET miss phase failed at operation %zu\n", i);
            goto cleanup;
        }
        checksum += (uint64_t)(i & 1U);
    }
    if (!monotonic_nanoseconds(&end)) {
        perror("clock_gettime");
        goto cleanup;
    }
    print_result("get-miss", operations, end - start);

    if (!monotonic_nanoseconds(&start)) {
        perror("clock_gettime");
        goto cleanup;
    }
    for (size_t i = 0; i < operations; i++) {
        size_t index = i % entries;
        bool created = true;
        kv_status status = kv_store_set(store, keys[index], strlen(keys[index]),
                                        "{\"value\":2}", update_value_len,
                                        &created);
        if (status != KV_STATUS_OK || created) {
            fprintf(stderr, "Update phase failed at operation %zu\n", i);
            goto cleanup;
        }
    }
    if (!monotonic_nanoseconds(&end)) {
        perror("clock_gettime");
        goto cleanup;
    }
    print_result("update", operations, end - start);

    if (!monotonic_nanoseconds(&start)) {
        perror("clock_gettime");
        goto cleanup;
    }
    for (size_t i = 0; i < entries; i++) {
        if (kv_store_delete(store, keys[i], strlen(keys[i])) != KV_STATUS_OK) {
            fprintf(stderr, "Delete phase failed at entry %zu\n", i);
            goto cleanup;
        }
    }
    if (!monotonic_nanoseconds(&end)) {
        perror("clock_gettime");
        goto cleanup;
    }
    print_result("delete", entries, end - start);
    printf("checksum=%" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    kv_store_destroy(store);
    free(keys);
    return result;
}
