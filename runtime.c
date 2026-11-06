/* Labels and manager identities are immutable. Recording updates thread-owned
 * aggregates; a reporter takes only the individual aggregate's short lock. */
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#if defined(__has_include) && __has_include(<runtime_abi.h>)
#include <runtime_abi.h>
#else
#include "../core/runtime_abi.h"
#endif
#if defined(_MSC_VER)
#define PROFILE_TLS __declspec(thread)
#else
#define PROFILE_TLS _Thread_local
#endif
#define PROFILE_BUCKETS 256u

typedef struct profile_manager profile_manager;
typedef struct profile_label profile_label;
typedef struct profile_sample profile_sample;
typedef struct profile_thread profile_thread;
struct profile_label {
    profile_manager *owner;
    char *text;
    size_t len, index;
    uint64_t hash;
    profile_label *next, *bucket_next;
};
struct profile_sample {
    profile_label *label;
    atomic_flag lock;
    uint64_t generation, count, total, min, max;
    profile_sample *next, *bucket_next;
};
struct profile_thread {
    profile_manager *manager;
    profile_sample *buckets[PROFILE_BUCKETS];
    profile_thread *tls_next, *owned_next;
};
struct profile_manager {
    char *name;
    size_t len, label_count;
    atomic_flag registry_lock;
    _Atomic uint64_t generation;
    profile_label *labels, *tail, *buckets[PROFILE_BUCKETS];
    _Atomic(profile_sample *) samples;
    profile_thread *threads;
    profile_manager *next;
};
static atomic_flag profile_registry_lock = ATOMIC_FLAG_INIT;
static profile_manager *profile_managers;
static PROFILE_TLS profile_thread *profile_threads;
static void lock(atomic_flag *value) {
    while (atomic_flag_test_and_set_explicit(value, memory_order_acquire)) {}
}
static void unlock(atomic_flag *value) { atomic_flag_clear_explicit(value, memory_order_release); }
static uint64_t hash_text(const char *value, size_t len) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; ++i) hash = (hash ^ (unsigned char)value[i]) * UINT64_C(1099511628211);
    return hash;
}
static uint64_t add(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }
static char *copy_text(const char *value, size_t len) {
    if (len == SIZE_MAX) return NULL;
    char *out = malloc(len + 1);
    if (out) { memcpy(out, value, len); out[len] = 0; }
    return out;
}
static void profile_cleanup(void) {
    for (profile_manager *m = profile_managers, *next; m; m = next) {
        next = m->next;
        for (profile_sample *s = atomic_load(&m->samples), *n; s; s = n) { n = s->next; free(s); }
        for (profile_label *l = m->labels, *n; l; l = n) { n = l->next; free(l->text); free(l); }
        for (profile_thread *t = m->threads, *n; t; t = n) { n = t->owned_next; free(t); }
        free(m->name); free(m);
    }
}
size_t encore_profile_manager_new(encore_str name) {
    const char *text = encore_str_data(name);
    const size_t len = encore_str_size(name);
    lock(&profile_registry_lock);
    for (profile_manager *m = profile_managers; m; m = m->next) {
        if (m->len == len && !memcmp(m->name, text, len)) {
            unlock(&profile_registry_lock); return (size_t)m;
        }
    }
    profile_manager *m = calloc(1, sizeof(*m));
    if (!m) { unlock(&profile_registry_lock); return 0; }
    m->name = copy_text(text, len);
    if (!m->name) { free(m); unlock(&profile_registry_lock); return 0; }
    m->len = len; atomic_flag_clear(&m->registry_lock);
    atomic_init(&m->generation, 1); atomic_init(&m->samples, NULL);
    if (!profile_managers) atexit(profile_cleanup);
    m->next = profile_managers; profile_managers = m;
    unlock(&profile_registry_lock); return (size_t)m;
}
static profile_label *register_label(profile_manager *m, const char *text, size_t len, uint64_t hash) {
    size_t bucket = hash % PROFILE_BUCKETS;
    lock(&m->registry_lock);
    for (profile_label *l = m->buckets[bucket]; l; l = l->bucket_next) {
        if (l->hash == hash && l->len == len && !memcmp(l->text, text, len)) {
            unlock(&m->registry_lock); return l;
        }
    }
    profile_label *l = calloc(1, sizeof(*l));
    if (!l) { unlock(&m->registry_lock); return NULL; }
    l->text = copy_text(text, len);
    if (!l->text) { free(l); unlock(&m->registry_lock); return NULL; }
    l->owner = m; l->len = len; l->hash = hash; l->index = m->label_count++;
    l->bucket_next = m->buckets[bucket]; m->buckets[bucket] = l;
    if (m->tail) m->tail->next = l; else m->labels = l;
    m->tail = l; unlock(&m->registry_lock); return l;
}
size_t encore_profile_register(size_t handle, encore_str name) {
    if (!handle) return 0;
    return (size_t)register_label((profile_manager *)handle, encore_str_data(name),
        encore_str_size(name), hash_text(encore_str_data(name), encore_str_size(name)));
}
static profile_thread *thread_for(profile_manager *m) {
    for (profile_thread *t = profile_threads; t; t = t->tls_next) if (t->manager == m) return t;
    profile_thread *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->manager = m; t->tls_next = profile_threads; profile_threads = t;
    lock(&m->registry_lock); t->owned_next = m->threads; m->threads = t; unlock(&m->registry_lock);
    return t;
}
static profile_sample *new_sample(profile_thread *t, profile_label *label) {
    profile_sample *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->label = label; s->min = UINT64_MAX; atomic_flag_clear(&s->lock);
    size_t bucket = label->hash % PROFILE_BUCKETS;
    s->bucket_next = t->buckets[bucket]; t->buckets[bucket] = s;
    profile_sample *head = atomic_load_explicit(&t->manager->samples, memory_order_acquire);
    do { s->next = head; } while (!atomic_compare_exchange_weak_explicit(
        &t->manager->samples, &head, s, memory_order_release, memory_order_acquire));
    return s;
}
static void record_sample(profile_sample *s, uint64_t elapsed) {
    if (!s) return;
    lock(&s->lock);
    uint64_t generation = atomic_load_explicit(&s->label->owner->generation, memory_order_acquire);
    if (s->generation != generation) {
        s->generation = generation; s->count = s->total = s->max = 0; s->min = UINT64_MAX;
    }
    s->count = add(s->count, 1); s->total = add(s->total, elapsed);
    if (elapsed < s->min) s->min = elapsed;
    if (elapsed > s->max) s->max = elapsed;
    unlock(&s->lock);
}
void encore_profile_record_registered(size_t handle, size_t token, uint64_t elapsed) {
    if (!handle || !token) return;
    profile_manager *m = (profile_manager *)handle;
    profile_label *label = (profile_label *)token;
    if (label->owner != m) return;
    profile_thread *t = thread_for(m);
    if (!t) return;
    for (profile_sample *s = t->buckets[label->hash % PROFILE_BUCKETS]; s; s = s->bucket_next) {
        if (s->label == label) { record_sample(s, elapsed); return; }
    }
    record_sample(new_sample(t, label), elapsed);
}
void encore_profile_record(size_t handle, encore_str name, uint64_t elapsed) {
    if (!handle) return;
    profile_manager *m = (profile_manager *)handle;
    profile_thread *t = thread_for(m);
    if (!t) return;
    const char *text = encore_str_data(name); size_t len = encore_str_size(name);
    uint64_t hash = hash_text(text, len);
    for (profile_sample *s = t->buckets[hash % PROFILE_BUCKETS]; s; s = s->bucket_next) {
        profile_label *l = s->label;
        if (l->hash == hash && l->len == len && !memcmp(l->text, text, len)) {
            record_sample(s, elapsed); return;
        }
    }
    profile_label *label = register_label(m, text, len, hash);
    if (label) record_sample(new_sample(t, label), elapsed);
}
typedef struct { profile_label *label; uint64_t count, total, min, max; } snapshot_entry;
static snapshot_entry *snapshot(profile_manager *m, size_t *size) {
    for (;;) {
        uint64_t generation = atomic_load_explicit(&m->generation, memory_order_acquire);
        lock(&m->registry_lock);
        size_t count = m->label_count;
        snapshot_entry *out = calloc(count ? count : 1, sizeof(*out));
        if (!out) { unlock(&m->registry_lock); return NULL; }
        for (profile_label *l = m->labels; l; l = l->next) { out[l->index].label = l; out[l->index].min = UINT64_MAX; }
        unlock(&m->registry_lock);
        for (profile_sample *s = atomic_load_explicit(&m->samples, memory_order_acquire); s; s = s->next) {
            if (s->label->index >= count) continue;
            snapshot_entry *e = &out[s->label->index];
            lock(&s->lock);
            if (s->generation == generation) {
                e->count = add(e->count, s->count); e->total = add(e->total, s->total);
                if (s->min < e->min) e->min = s->min;
                if (s->max > e->max) e->max = s->max;
            }
            unlock(&s->lock);
        }
        if (generation == atomic_load_explicit(&m->generation, memory_order_acquire)) { *size = count; return out; }
        free(out);
    }
}
encore_str encore_profile_report(size_t handle) {
    if (!handle) return encore_empty_str();
    profile_manager *m = (profile_manager *)handle;
    size_t count; snapshot_entry *entries = snapshot(m, &count);
    if (!entries) return encore_empty_str();
    size_t capacity = m->len + 1;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].count) {
            if (entries[i].label->len > SIZE_MAX - 256 || capacity > SIZE_MAX - entries[i].label->len - 256) {
                free(entries); return encore_empty_str();
            }
            capacity += entries[i].label->len + 256;
        }
    }
    char *buffer = malloc(capacity);
    if (!buffer) { free(entries); return encore_empty_str(); }
    memcpy(buffer, m->name, m->len); size_t used = m->len;
    for (size_t i = 0; i < count; ++i) {
        snapshot_entry *e = &entries[i]; if (!e->count) continue;
        buffer[used++] = '\n'; memcpy(buffer + used, e->label->text, e->label->len); used += e->label->len;
        int written = snprintf(buffer + used, capacity - used,
            ": count=%" PRIu64 " total=%" PRIu64 "ns avg=%" PRIu64 "ns min=%" PRIu64 "ns max=%" PRIu64 "ns",
            e->count, e->total, e->total / e->count, e->min, e->max);
        if (written < 0 || (size_t)written >= capacity - used) { free(buffer); free(entries); return encore_empty_str(); }
        used += (size_t)written;
    }
    buffer[used] = 0; free(entries); return encore_from_owned_buffer(buffer, used);
}
void encore_profile_reset(size_t handle) {
    if (handle) atomic_fetch_add_explicit(&((profile_manager *)handle)->generation, 1, memory_order_acq_rel);
}
uint64_t encore_profile_sample_count(size_t handle) {
    if (!handle) return 0;
    size_t count; snapshot_entry *entries = snapshot((profile_manager *)handle, &count);
    if (!entries) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) total = add(total, entries[i].count);
    free(entries); return total;
}
