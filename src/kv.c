#define _POSIX_C_SOURCE 200809L

#include "kv.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KV_INITIAL_CAPACITY 16U
#define KV_LOAD_NUMERATOR 7U
#define KV_LOAD_DENOMINATOR 10U

#define XXH64_PRIME1 UINT64_C(11400714785074694791)
#define XXH64_PRIME2 UINT64_C(14029467366897019727)
#define XXH64_PRIME3 UINT64_C(1609587929392839161)
#define XXH64_PRIME4 UINT64_C(9650029242287828579)
#define XXH64_PRIME5 UINT64_C(2870177450012600261)

typedef enum {
    KV_ENTRY_EMPTY = 0,
    KV_ENTRY_FULL,
    KV_ENTRY_TOMBSTONE
} kv_entry_state;

typedef struct {
    char *key;
    size_t key_len;
    char *value;
    size_t value_len;
    uint64_t hash;
    uint64_t expires_at_ms;
    kv_entry_state state;
} kv_entry;

struct kv_store {
    kv_entry *entries;
    size_t capacity;
    size_t key_count;
    size_t tombstone_count;
    size_t stored_bytes;
    uint64_t set_count;
    uint64_t get_count;
    uint64_t delete_count;
    uint64_t next_expiration_ms;
};

typedef struct {
    size_t index;
    bool found;
} kv_slot;

static bool monotonic_millis(uint64_t *milliseconds) {
    struct timespec now;
    uint64_t seconds;
    uint64_t base;
    uint64_t fraction;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
        return false;
    }
    seconds = (uint64_t)now.tv_sec;
    if (seconds > UINT64_MAX / UINT64_C(1000)) {
        return false;
    }
    base = seconds * UINT64_C(1000);
    fraction = (uint64_t)now.tv_nsec / UINT64_C(1000000);
    if (fraction > UINT64_MAX - base) {
        return false;
    }
    *milliseconds = base + fraction;
    return true;
}

static kv_status get_expiration_deadline(uint64_t ttl_seconds,
                                         uint64_t *deadline) {
    uint64_t now;

    if (ttl_seconds == 0U) {
        *deadline = 0U;
        return KV_STATUS_OK;
    }
    if (!monotonic_millis(&now)) {
        return KV_STATUS_INTERNAL_ERROR;
    }
    if (ttl_seconds > (UINT64_MAX - now) / UINT64_C(1000)) {
        return KV_STATUS_INVALID_ARGUMENT;
    }
    *deadline = now + ttl_seconds * UINT64_C(1000);
    return KV_STATUS_OK;
}

static void remove_entry(kv_store *store,
                         kv_entry *entry,
                         bool count_as_delete) {
    free(entry->key);
    free(entry->value);
    store->stored_bytes -= entry->key_len + entry->value_len;
    entry->key = NULL;
    entry->key_len = 0U;
    entry->value = NULL;
    entry->value_len = 0U;
    entry->hash = 0U;
    entry->expires_at_ms = 0U;
    entry->state = KV_ENTRY_TOMBSTONE;
    store->key_count--;
    store->tombstone_count++;
    if (count_as_delete) {
        store->delete_count++;
    }
}

static void expire_due_entries(kv_store *store) {
    uint64_t now;
    uint64_t next_expiration = 0U;

    if (store->next_expiration_ms == 0U ||
        !monotonic_millis(&now) || now < store->next_expiration_ms) {
        return;
    }

    for (size_t i = 0U; i < store->capacity; i++) {
        kv_entry *entry = &store->entries[i];
        if (entry->state != KV_ENTRY_FULL || entry->expires_at_ms == 0U) {
            continue;
        }
        if (now >= entry->expires_at_ms) {
            remove_entry(store, entry, false);
        } else if (next_expiration == 0U ||
                   entry->expires_at_ms < next_expiration) {
            next_expiration = entry->expires_at_ms;
        }
    }
    store->next_expiration_ms = next_expiration;
}

static uint64_t rotate_left_64(uint64_t value, unsigned int amount) {
    return (value << amount) | (value >> (64U - amount));
}

static uint64_t read_u64_le(const unsigned char *data) {
    uint64_t value = 0;

    for (size_t i = 0; i < 8; i++) {
        value |= (uint64_t)data[i] << (i * 8U);
    }
    return value;
}

static uint32_t read_u32_le(const unsigned char *data) {
    uint32_t value = 0;

    for (size_t i = 0; i < 4; i++) {
        value |= (uint32_t)data[i] << (i * 8U);
    }
    return value;
}

static uint64_t xxhash_round(uint64_t accumulator, uint64_t input) {
    accumulator += input * XXH64_PRIME2;
    accumulator = rotate_left_64(accumulator, 31U);
    return accumulator * XXH64_PRIME1;
}

static uint64_t xxhash_merge_round(uint64_t accumulator, uint64_t value) {
    accumulator ^= xxhash_round(0, value);
    return accumulator * XXH64_PRIME1 + XXH64_PRIME4;
}

static uint64_t hash_bytes(const char *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t offset = 0;
    uint64_t hash;

    if (length >= 32U) {
        uint64_t v1 = XXH64_PRIME1 + XXH64_PRIME2;
        uint64_t v2 = XXH64_PRIME2;
        uint64_t v3 = 0;
        uint64_t v4 = 0 - XXH64_PRIME1;
        size_t limit = length - 32U;

        do {
            v1 = xxhash_round(v1, read_u64_le(bytes + offset));
            v2 = xxhash_round(v2, read_u64_le(bytes + offset + 8U));
            v3 = xxhash_round(v3, read_u64_le(bytes + offset + 16U));
            v4 = xxhash_round(v4, read_u64_le(bytes + offset + 24U));
            offset += 32U;
        } while (offset <= limit);

        hash = rotate_left_64(v1, 1U) + rotate_left_64(v2, 7U) +
               rotate_left_64(v3, 12U) + rotate_left_64(v4, 18U);
        hash = xxhash_merge_round(hash, v1);
        hash = xxhash_merge_round(hash, v2);
        hash = xxhash_merge_round(hash, v3);
        hash = xxhash_merge_round(hash, v4);
    } else {
        hash = XXH64_PRIME5;
    }

    hash += (uint64_t)length;

    while (length - offset >= 8U) {
        uint64_t value = xxhash_round(0, read_u64_le(bytes + offset));
        hash ^= value;
        hash = rotate_left_64(hash, 27U) * XXH64_PRIME1 + XXH64_PRIME4;
        offset += 8U;
    }
    if (length - offset >= 4U) {
        hash ^= (uint64_t)read_u32_le(bytes + offset) * XXH64_PRIME1;
        hash = rotate_left_64(hash, 23U) * XXH64_PRIME2 + XXH64_PRIME3;
        offset += 4U;
    }
    while (offset < length) {
        hash ^= (uint64_t)bytes[offset] * XXH64_PRIME5;
        hash = rotate_left_64(hash, 11U) * XXH64_PRIME1;
        offset++;
    }

    hash ^= hash >> 33U;
    hash *= XXH64_PRIME2;
    hash ^= hash >> 29U;
    hash *= XXH64_PRIME3;
    hash ^= hash >> 32U;
    return hash;
}

static kv_slot find_slot(const kv_store *store,
                         const char *key,
                         size_t key_len,
                         uint64_t hash) {
    size_t first_tombstone = SIZE_MAX;
    size_t mask = store->capacity - 1U;

    for (size_t probe = 0; probe < store->capacity; probe++) {
        size_t index = ((size_t)hash + probe) & mask;
        const kv_entry *entry = &store->entries[index];

        if (entry->state == KV_ENTRY_EMPTY) {
            kv_slot slot = {
                .index = first_tombstone == SIZE_MAX ? index : first_tombstone,
                .found = false
            };
            return slot;
        }
        if (entry->state == KV_ENTRY_TOMBSTONE) {
            if (first_tombstone == SIZE_MAX) {
                first_tombstone = index;
            }
            continue;
        }
        if (entry->hash == hash && entry->key_len == key_len &&
            memcmp(entry->key, key, key_len) == 0) {
            kv_slot slot = {.index = index, .found = true};
            return slot;
        }
    }

    kv_slot slot = {.index = first_tombstone, .found = false};
    return slot;
}

static bool rehash(kv_store *store, size_t capacity) {
    if (capacity < KV_INITIAL_CAPACITY ||
        capacity > SIZE_MAX / sizeof(kv_entry)) {
        return false;
    }

    kv_entry *entries = calloc(capacity, sizeof(*entries));
    if (entries == NULL) {
        return false;
    }

    size_t mask = capacity - 1U;
    for (size_t i = 0; i < store->capacity; i++) {
        kv_entry *old_entry = &store->entries[i];
        if (old_entry->state != KV_ENTRY_FULL) {
            continue;
        }

        size_t index = (size_t)old_entry->hash & mask;
        while (entries[index].state == KV_ENTRY_FULL) {
            index = (index + 1U) & mask;
        }
        entries[index] = *old_entry;
    }

    free(store->entries);
    store->entries = entries;
    store->capacity = capacity;
    store->tombstone_count = 0;
    return true;
}

static bool prepare_insert(kv_store *store) {
    size_t used = store->key_count + store->tombstone_count + 1U;
    size_t threshold = store->capacity * KV_LOAD_NUMERATOR /
                       KV_LOAD_DENOMINATOR;

    if (used <= threshold) {
        return true;
    }
    if (store->key_count + 1U <= threshold) {
        return rehash(store, store->capacity);
    }
    if (store->capacity > SIZE_MAX / 2U) {
        return false;
    }
    return rehash(store, store->capacity * 2U);
}

kv_store *kv_store_create(void) {
    kv_store *store = calloc(1, sizeof(*store));
    if (store == NULL) {
        return NULL;
    }
    if (!rehash(store, KV_INITIAL_CAPACITY)) {
        free(store);
        return NULL;
    }
    return store;
}

void kv_store_destroy(kv_store *store) {
    if (store == NULL) {
        return;
    }

    for (size_t i = 0; i < store->capacity; i++) {
        if (store->entries[i].state == KV_ENTRY_FULL) {
            free(store->entries[i].key);
            free(store->entries[i].value);
        }
    }
    free(store->entries);
    free(store);
}

kv_status kv_store_set(kv_store *store,
                       const char *key,
                       size_t key_len,
                       const char *value,
                       size_t value_len,
                       bool *created) {
    return kv_store_set_with_ttl(store, key, key_len, value, value_len, 0U,
                                 created);
}

kv_status kv_store_set_with_ttl(kv_store *store,
                                const char *key,
                                size_t key_len,
                                const char *value,
                                size_t value_len,
                                uint64_t ttl_seconds,
                                bool *created) {
    uint64_t expires_at_ms;
    kv_status expiration_status;

    if (store == NULL || key == NULL || key_len == 0U || key_len == SIZE_MAX ||
        value == NULL || value_len == 0U || value_len == SIZE_MAX) {
        return KV_STATUS_INVALID_ARGUMENT;
    }

    expire_due_entries(store);
    expiration_status = get_expiration_deadline(ttl_seconds, &expires_at_ms);
    if (expiration_status != KV_STATUS_OK) {
        return expiration_status;
    }

    uint64_t hash = hash_bytes(key, key_len);
    kv_slot slot = find_slot(store, key, key_len, hash);

    if (slot.found) {
        kv_entry *entry = &store->entries[slot.index];
        size_t existing_bytes = store->stored_bytes - entry->value_len;
        if (value_len > SIZE_MAX - existing_bytes) {
            return KV_STATUS_NO_MEMORY;
        }

        char *value_copy = malloc(value_len + 1U);
        if (value_copy == NULL) {
            return KV_STATUS_NO_MEMORY;
        }
        memcpy(value_copy, value, value_len);
        value_copy[value_len] = '\0';

        free(entry->value);
        entry->value = value_copy;
        entry->value_len = value_len;
        entry->expires_at_ms = expires_at_ms;
        if (expires_at_ms != 0U &&
            (store->next_expiration_ms == 0U ||
             expires_at_ms < store->next_expiration_ms)) {
            store->next_expiration_ms = expires_at_ms;
        }
        store->stored_bytes = existing_bytes + value_len;
        store->set_count++;
        if (created != NULL) {
            *created = false;
        }
        return KV_STATUS_OK;
    }

    if (!prepare_insert(store)) {
        return KV_STATUS_NO_MEMORY;
    }
    if (key_len > SIZE_MAX - value_len ||
        key_len + value_len > SIZE_MAX - store->stored_bytes) {
        return KV_STATUS_NO_MEMORY;
    }

    char *key_copy = malloc(key_len + 1U);
    char *value_copy = malloc(value_len + 1U);
    if (key_copy == NULL || value_copy == NULL) {
        free(key_copy);
        free(value_copy);
        return KV_STATUS_NO_MEMORY;
    }
    memcpy(key_copy, key, key_len);
    key_copy[key_len] = '\0';
    memcpy(value_copy, value, value_len);
    value_copy[value_len] = '\0';

    slot = find_slot(store, key, key_len, hash);
    if (slot.index == SIZE_MAX) {
        free(key_copy);
        free(value_copy);
        return KV_STATUS_NO_MEMORY;
    }

    kv_entry *entry = &store->entries[slot.index];
    if (entry->state == KV_ENTRY_TOMBSTONE) {
        store->tombstone_count--;
    }
    entry->key = key_copy;
    entry->key_len = key_len;
    entry->value = value_copy;
    entry->value_len = value_len;
    entry->hash = hash;
    entry->expires_at_ms = expires_at_ms;
    entry->state = KV_ENTRY_FULL;
    if (expires_at_ms != 0U &&
        (store->next_expiration_ms == 0U ||
         expires_at_ms < store->next_expiration_ms)) {
        store->next_expiration_ms = expires_at_ms;
    }
    store->key_count++;
    store->stored_bytes += key_len + value_len;
    store->set_count++;
    if (created != NULL) {
        *created = true;
    }
    return KV_STATUS_OK;
}

kv_status kv_store_get(kv_store *store,
                       const char *key,
                       size_t key_len,
                       const char **value,
                       size_t *value_len) {
    if (store == NULL || key == NULL || key_len == 0 || value == NULL ||
        value_len == NULL) {
        return KV_STATUS_INVALID_ARGUMENT;
    }

    expire_due_entries(store);
    store->get_count++;
    kv_slot slot = find_slot(store, key, key_len, hash_bytes(key, key_len));
    if (!slot.found) {
        return KV_STATUS_NOT_FOUND;
    }

    const kv_entry *entry = &store->entries[slot.index];
    *value = entry->value;
    *value_len = entry->value_len;
    return KV_STATUS_OK;
}

kv_status kv_store_delete(kv_store *store, const char *key, size_t key_len) {
    if (store == NULL || key == NULL || key_len == 0) {
        return KV_STATUS_INVALID_ARGUMENT;
    }

    expire_due_entries(store);
    kv_slot slot = find_slot(store, key, key_len, hash_bytes(key, key_len));
    if (!slot.found) {
        return KV_STATUS_NOT_FOUND;
    }

    remove_entry(store, &store->entries[slot.index], true);
    return KV_STATUS_OK;
}

bool kv_store_visit(kv_store *store, kv_visit_fn visitor, void *context) {
    if (store == NULL || visitor == NULL) {
        return false;
    }

    expire_due_entries(store);
    for (size_t i = 0; i < store->capacity; i++) {
        const kv_entry *entry = &store->entries[i];
        if (entry->state == KV_ENTRY_FULL &&
            !visitor(entry->key, entry->key_len, context)) {
            return false;
        }
    }
    return true;
}

void kv_store_get_stats(kv_store *store, kv_stats *stats) {
    if (store == NULL || stats == NULL) {
        return;
    }

    expire_due_entries(store);
    stats->key_count = store->key_count;
    stats->capacity = store->capacity;
    stats->stored_bytes = store->stored_bytes;
    stats->set_count = store->set_count;
    stats->get_count = store->get_count;
    stats->delete_count = store->delete_count;
}
