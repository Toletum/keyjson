#ifndef KEYJSON_KV_H
#define KEYJSON_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct kv_store kv_store;

typedef enum {
    KV_STATUS_OK = 0,
    KV_STATUS_NOT_FOUND,
    KV_STATUS_NO_MEMORY,
    KV_STATUS_INVALID_ARGUMENT,
    KV_STATUS_INTERNAL_ERROR
} kv_status;

typedef struct {
    size_t key_count;
    size_t capacity;
    size_t stored_bytes;
    uint64_t set_count;
    uint64_t get_count;
    uint64_t delete_count;
} kv_stats;

typedef bool (*kv_visit_fn)(const char *key, size_t key_len, void *context);

kv_store *kv_store_create(void);
void kv_store_destroy(kv_store *store);

kv_status kv_store_set(kv_store *store,
                       const char *key,
                       size_t key_len,
                       const char *value,
                       size_t value_len,
                       bool *created);
kv_status kv_store_set_with_ttl(kv_store *store,
                                const char *key,
                                size_t key_len,
                                const char *value,
                                size_t value_len,
                                uint64_t ttl_seconds,
                                bool *created);
kv_status kv_store_get(kv_store *store,
                       const char *key,
                       size_t key_len,
                       const char **value,
                       size_t *value_len);
kv_status kv_store_delete(kv_store *store, const char *key, size_t key_len);

bool kv_store_visit(kv_store *store, kv_visit_fn visitor, void *context);
void kv_store_get_stats(kv_store *store, kv_stats *stats);

#endif
