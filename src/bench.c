#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "internal.h"

#include <stdlib.h>
#include <time.h>

#define BENCH_SLOTS 4096

typedef struct bench_ops {
    void *(*alloc)(void *ctx, size_t size);
    void *(*resize)(void *ctx, void *ptr, size_t size);
    void (*release)(void *ctx, void *ptr);
    void *ctx;
} bench_ops;

static double now_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER f;
    LARGE_INTEGER c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static uint64_t next_random(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static size_t pick_size(uint64_t *state, size_t min_size, size_t max_size)
{
    if (max_size <= min_size)
        return min_size;
    uint64_t r = next_random(state);
    size_t span = max_size - min_size;
    if (r & 1)
        return min_size + (size_t)((r >> 8) % (span / 8 + 1));
    return min_size + (size_t)((r >> 8) % (span + 1));
}

static void *heap_alloc(void *ctx, size_t size)
{
    return favo_heap_alloc((favo_heap *)ctx, size);
}

static void *heap_resize(void *ctx, void *ptr, size_t size)
{
    return favo_heap_realloc((favo_heap *)ctx, ptr, size);
}

static void heap_release(void *ctx, void *ptr)
{
    favo_heap_free((favo_heap *)ctx, ptr);
}

static void *sys_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void *sys_resize(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    return realloc(ptr, size);
}

static void sys_release(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

static int run(const bench_ops *ops, uint64_t operations, uint64_t seed, size_t min_size, size_t max_size, favo_bench_result *out, favo_heap *heap)
{
    void **slots = (void **)calloc(BENCH_SLOTS, sizeof(void *));
    size_t *sizes = (size_t *)calloc(BENCH_SLOTS, sizeof(size_t));
    if (!slots || !sizes) {
        free(slots);
        free(sizes);
        return FAVO_ENOMEM;
    }
    uint64_t state = seed ? seed : 0x9E3779B97F4A7C15ull;
    uint64_t failed = 0;
    favo_stats peak;
    memset(&peak, 0, sizeof(peak));
    double start = now_seconds();
    for (uint64_t i = 0; i < operations; i++) {
        size_t slot = (size_t)(next_random(&state) % BENCH_SLOTS);
        if (!slots[slot]) {
            size_t size = pick_size(&state, min_size, max_size);
            unsigned char *p = (unsigned char *)ops->alloc(ops->ctx, size);
            if (!p) {
                failed++;
                continue;
            }
            p[0] = (unsigned char)i;
            p[size - 1] = (unsigned char)i;
            slots[slot] = p;
            sizes[slot] = size;
        } else if (next_random(&state) % 10 < 7) {
            ops->release(ops->ctx, slots[slot]);
            slots[slot] = NULL;
        } else {
            size_t size = pick_size(&state, min_size, max_size);
            unsigned char *p = (unsigned char *)ops->resize(ops->ctx, slots[slot], size);
            if (!p) {
                failed++;
                continue;
            }
            p[size - 1] = (unsigned char)i;
            slots[slot] = p;
            sizes[slot] = size;
        }
        if (heap && (i + 1) == operations / 2)
            favo_heap_stats(heap, &peak);
    }
    double elapsed = now_seconds() - start;
    if (heap && !peak.arena_bytes)
        favo_heap_stats(heap, &peak);
    for (size_t s = 0; s < BENCH_SLOTS; s++)
        if (slots[s])
            ops->release(ops->ctx, slots[s]);
    free(slots);
    free(sizes);
    out->operations = operations;
    out->seconds = elapsed;
    out->ops_per_second = elapsed > 0 ? (double)operations / elapsed : 0.0;
    out->failed = failed;
    if (heap) {
        favo_stats end;
        favo_heap_stats(heap, &end);
        out->peak_arena_bytes = end.peak_arena_bytes;
        out->fragmentation = peak.fragmentation;
        out->utilization = peak.utilization;
        out->search_steps = end.search_steps;
    }
    return FAVO_OK;
}

int favo_bench_run(int policy, uint64_t operations, uint64_t seed, size_t min_size, size_t max_size, favo_bench_result *out)
{
    if (!out || !min_size || max_size < min_size)
        return FAVO_EINVAL;
    memset(out, 0, sizeof(*out));
    favo_config cfg;
    favo_config_default(&cfg);
    cfg.policy = policy;
    cfg.source = FAVO_SOURCE_MMAP;
    cfg.thread_safe = 0;
    favo_heap *h = favo_heap_create(&cfg);
    if (!h)
        return FAVO_ENOMEM;
    bench_ops ops = { heap_alloc, heap_resize, heap_release, h };
    out->policy = policy;
    int rc = run(&ops, operations, seed, min_size, max_size, out, h);
    favo_heap_destroy(h);
    return rc;
}

int favo_bench_system(uint64_t operations, uint64_t seed, size_t min_size, size_t max_size, favo_bench_result *out)
{
    if (!out || !min_size || max_size < min_size)
        return FAVO_EINVAL;
    memset(out, 0, sizeof(*out));
    bench_ops ops = { sys_alloc, sys_resize, sys_release, NULL };
    out->policy = -1;
    return run(&ops, operations, seed, min_size, max_size, out, NULL);
}
