#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define FAVO_EXPORT __attribute__((visibility("default")))

static void before_fork(void)
{
    favo_lock_acquire(&favo_default_heap()->lock);
}

static void after_fork(void)
{
    favo_lock_release(&favo_default_heap()->lock);
}

static void write_stats(void)
{
    favo_stats s;
    favo_heap_stats(favo_default_heap(), &s);
    char line[512];
    int n = snprintf(line, sizeof(line),
        "favo[%d] politica=%s arenas=%zu bytes_arena=%zu pico=%zu ocupados=%zu livres=%zu "
        "alocacoes=%llu liberacoes=%llu divisoes=%llu fusoes=%llu fragmentacao=%.3f\n",
        (int)getpid(), favo_policy_name(favo_default_heap()->cfg.policy), s.arena_count, s.arena_bytes,
        s.peak_arena_bytes, s.used_blocks, s.free_blocks, (unsigned long long)s.total_allocs,
        (unsigned long long)s.total_frees, (unsigned long long)s.splits,
        (unsigned long long)s.coalesces, s.fragmentation);
    if (n > 0) {
        ssize_t ignored = write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
        (void)ignored;
    }
}

static void report_stats(void)
{
    const char *v = getenv("FAVO_STATS");
    if (!v || *v == '0')
        return;
    write_stats();
}

__attribute__((constructor)) static void favo_preload_init(void)
{
    favo_default_heap();
    pthread_atfork(before_fork, after_fork, after_fork);
}

__attribute__((destructor)) static void favo_preload_fini(void)
{
    report_stats();
}

FAVO_EXPORT void *malloc(size_t size)
{
    return favo_malloc(size);
}

FAVO_EXPORT void free(void *ptr)
{
    favo_free(ptr);
}

FAVO_EXPORT void *calloc(size_t count, size_t size)
{
    return favo_calloc(count, size);
}

FAVO_EXPORT void *realloc(void *ptr, size_t size)
{
    return favo_realloc(ptr, size);
}

FAVO_EXPORT void *reallocarray(void *ptr, size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return favo_realloc(ptr, count * size);
}

FAVO_EXPORT void *memalign(size_t alignment, size_t size)
{
    return favo_memalign(alignment, size);
}

FAVO_EXPORT int posix_memalign(void **out, size_t alignment, size_t size)
{
    return favo_posix_memalign(out, alignment, size);
}

FAVO_EXPORT void *aligned_alloc(size_t alignment, size_t size)
{
    return favo_aligned_alloc(alignment, size);
}

FAVO_EXPORT void *valloc(size_t size)
{
    return favo_memalign(favo_page_size(), size);
}

FAVO_EXPORT void *pvalloc(size_t size)
{
    size_t page = favo_page_size();
    return favo_memalign(page, favo_align_up(size ? size : 1, page));
}

FAVO_EXPORT size_t malloc_usable_size(void *ptr)
{
    return favo_usable_size(ptr);
}

/* Extensões da glibc: sem elas, malloc_trim e malloc_stats caem na glibc,
   que só enxerga as arenas dela e ignora o heap do Favo. */
FAVO_EXPORT int malloc_trim(size_t pad)
{
    return favo_heap_trim(favo_default_heap(), pad) > 0;
}

FAVO_EXPORT void malloc_stats(void)
{
    write_stats();
}
