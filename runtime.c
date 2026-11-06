#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

typedef struct {
    size_t ref_count;
    size_t len;
    char data[];
} encore_str_object;

typedef struct {
    encore_str_object *object;
} encore_str;

extern encore_str encore_from_owned_buffer(char *buffer, size_t len);
extern encore_str encore_empty_str(void);

typedef struct {
    char *label;
    size_t label_len;
    uint64_t count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
} encore_profile_entry;

typedef struct {
    char *name;
    size_t name_len;
    encore_profile_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
} encore_profile_manager;

static atomic_flag encore_profile_lock = ATOMIC_FLAG_INIT;
static encore_profile_manager *encore_profile_managers = NULL;
static size_t encore_profile_manager_count = 0;
static size_t encore_profile_manager_capacity = 0;

static void profile_cleanup(void) {
    for (size_t manager_index = encore_profile_manager_count; manager_index > 0; --manager_index) {
        encore_profile_manager *manager = &encore_profile_managers[manager_index - 1];
        for (size_t entry_index = manager->entry_count; entry_index > 0; --entry_index) {
            free(manager->entries[entry_index - 1].label);
        }
        free(manager->entries);
        free(manager->name);
    }
    free(encore_profile_managers);
    encore_profile_managers = NULL;
    encore_profile_manager_count = 0;
    encore_profile_manager_capacity = 0;
}

static void profile_lock(void) {
    while (atomic_flag_test_and_set_explicit(&encore_profile_lock, memory_order_acquire)) {
    }
}

static void profile_unlock(void) {
    atomic_flag_clear_explicit(&encore_profile_lock, memory_order_release);
}

static const char *profile_str_data(encore_str value) {
    return value.object == NULL ? "" : value.object->data;
}

static size_t profile_str_len(encore_str value) {
    return value.object == NULL ? 0 : value.object->len;
}

static int profile_text_equal(const char *left, size_t left_len,
                              const char *right, size_t right_len) {
    return left_len == right_len &&
           (left_len == 0 || memcmp(left, right, left_len) == 0);
}

static char *profile_text_copy(const char *value, size_t len) {
    char *out = (char *)malloc(len + 1);
    if (out == NULL) return NULL;
    if (len != 0) memcpy(out, value, len);
    out[len] = '\0';
    return out;
}

size_t encore_profile_manager_new(encore_str name) {
    const char *name_data = profile_str_data(name);
    const size_t name_len = profile_str_len(name);
    profile_lock();
    for (size_t index = 0; index < encore_profile_manager_count; ++index) {
        encore_profile_manager *manager = &encore_profile_managers[index];
        if (profile_text_equal(manager->name, manager->name_len, name_data, name_len)) {
            profile_unlock();
            return index + 1;
        }
    }

    if (encore_profile_manager_count == encore_profile_manager_capacity) {
        const size_t next_capacity = encore_profile_manager_capacity == 0
            ? 4 : encore_profile_manager_capacity * 2;
        void *next = realloc(encore_profile_managers,
                             next_capacity * sizeof(encore_profile_manager));
        if (next == NULL) {
            profile_unlock();
            return 0;
        }
        encore_profile_managers = (encore_profile_manager *)next;
        encore_profile_manager_capacity = next_capacity;
    }

    char *owned_name = profile_text_copy(name_data, name_len);
    if (owned_name == NULL) {
        profile_unlock();
        return 0;
    }
    encore_profile_manager manager;
    manager.name = owned_name;
    manager.name_len = name_len;
    manager.entries = NULL;
    manager.entry_count = 0;
    manager.entry_capacity = 0;
    if (encore_profile_manager_count == 0) {
        (void)atexit(profile_cleanup);
    }
    encore_profile_managers[encore_profile_manager_count] = manager;
    encore_profile_manager_count += 1;
    const size_t handle = encore_profile_manager_count;
    profile_unlock();
    return handle;
}

static encore_profile_entry *profile_entry_for(
    encore_profile_manager *manager, const char *label, size_t label_len) {
    for (size_t index = 0; index < manager->entry_count; ++index) {
        encore_profile_entry *entry = &manager->entries[index];
        if (profile_text_equal(entry->label, entry->label_len, label, label_len)) {
            return entry;
        }
    }
    if (manager->entry_count == manager->entry_capacity) {
        const size_t next_capacity = manager->entry_capacity == 0
            ? 8 : manager->entry_capacity * 2;
        void *next = realloc(manager->entries,
                             next_capacity * sizeof(encore_profile_entry));
        if (next == NULL) return NULL;
        manager->entries = (encore_profile_entry *)next;
        manager->entry_capacity = next_capacity;
    }
    char *owned_label = profile_text_copy(label, label_len);
    if (owned_label == NULL) return NULL;
    encore_profile_entry entry;
    entry.label = owned_label;
    entry.label_len = label_len;
    entry.count = 0;
    entry.total_ns = 0;
    entry.min_ns = UINT64_MAX;
    entry.max_ns = 0;
    manager->entries[manager->entry_count] = entry;
    manager->entry_count += 1;
    return &manager->entries[manager->entry_count - 1];
}

void encore_profile_record(size_t handle, encore_str label, uint64_t elapsed_ns) {
    if (handle == 0) return;
    profile_lock();
    if (handle > encore_profile_manager_count) {
        profile_unlock();
        return;
    }
    encore_profile_manager *manager = &encore_profile_managers[handle - 1];
    encore_profile_entry *entry = profile_entry_for(
        manager, profile_str_data(label), profile_str_len(label));
    if (entry != NULL) {
        entry->count += 1;
        entry->total_ns = UINT64_MAX - entry->total_ns < elapsed_ns
            ? UINT64_MAX : entry->total_ns + elapsed_ns;
        if (elapsed_ns < entry->min_ns) entry->min_ns = elapsed_ns;
        if (elapsed_ns > entry->max_ns) entry->max_ns = elapsed_ns;
    }
    profile_unlock();
}

static size_t profile_decimal_len(uint64_t value) {
    size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        digits += 1;
    }
    return digits;
}

encore_str encore_profile_report(size_t handle) {
    if (handle == 0) return encore_empty_str();
    profile_lock();
    if (handle > encore_profile_manager_count) {
        profile_unlock();
        return encore_empty_str();
    }
    encore_profile_manager *manager = &encore_profile_managers[handle - 1];
    size_t capacity = manager->name_len + 32;
    for (size_t index = 0; index < manager->entry_count; ++index) {
        encore_profile_entry *entry = &manager->entries[index];
        capacity += entry->label_len + 96 +
            profile_decimal_len(entry->count) +
            profile_decimal_len(entry->total_ns) +
            profile_decimal_len(entry->min_ns) +
            profile_decimal_len(entry->max_ns);
    }
    char *buffer = (char *)malloc(capacity + 1);
    if (buffer == NULL) {
        profile_unlock();
        return encore_empty_str();
    }
    size_t used = (size_t)snprintf(buffer, capacity + 1, "%.*s",
                                   (int)manager->name_len, manager->name);
    for (size_t index = 0; index < manager->entry_count && used < capacity; ++index) {
        encore_profile_entry *entry = &manager->entries[index];
        const uint64_t average = entry->count == 0 ? 0 : entry->total_ns / entry->count;
        const int written = snprintf(
            buffer + used, capacity + 1 - used,
            "\n%.*s: count=%" PRIu64 " total=%" PRIu64
            "ns avg=%" PRIu64 "ns min=%" PRIu64 "ns max=%" PRIu64 "ns",
            (int)entry->label_len, entry->label, entry->count, entry->total_ns,
            average, entry->min_ns == UINT64_MAX ? 0 : entry->min_ns, entry->max_ns);
        if (written < 0) break;
        used += (size_t)written;
    }
    profile_unlock();
    buffer[used] = '\0';
    return encore_from_owned_buffer(buffer, used);
}

void encore_profile_reset(size_t handle) {
    if (handle == 0) return;
    profile_lock();
    if (handle <= encore_profile_manager_count) {
        encore_profile_manager *manager = &encore_profile_managers[handle - 1];
        for (size_t index = 0; index < manager->entry_count; ++index) {
            free(manager->entries[index].label);
        }
        free(manager->entries);
        manager->entries = NULL;
        manager->entry_count = 0;
        manager->entry_capacity = 0;
    }
    profile_unlock();
}

uint64_t encore_profile_sample_count(size_t handle) {
    if (handle == 0) return 0;
    profile_lock();
    uint64_t total = 0;
    if (handle <= encore_profile_manager_count) {
        encore_profile_manager *manager = &encore_profile_managers[handle - 1];
        for (size_t index = 0; index < manager->entry_count; ++index) {
            const uint64_t count = manager->entries[index].count;
            total = UINT64_MAX - total < count ? UINT64_MAX : total + count;
        }
    }
    profile_unlock();
    return total;
}
