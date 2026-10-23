#include "test.h"

#include <stdint.h>

static void alloc_and_free(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    unsigned char *p = favo_heap_alloc(h, 100);
    CHECK(p != NULL);
    CHECK(((uintptr_t)p % FAVO_ALIGNMENT) == 0);
    memset(p, 0xAB, 100);
    CHECK(favo_heap_usable_size(h, p) >= 100);
    CHECK(favo_heap_owns(h, p));
    CHECK_HEAP(h);
    CHECK(favo_heap_free(h, p) == FAVO_OK);
    CHECK_HEAP(h);
    favo_heap_destroy(h);
}

static void alignment_of_every_size(void)
{
    favo_heap *h = test_heap(FAVO_BEST_FIT);
    void *ptrs[300];
    for (size_t i = 0; i < 300; i++) {
        ptrs[i] = favo_heap_alloc(h, i);
        CHECK(ptrs[i] != NULL);
        CHECK(((uintptr_t)ptrs[i] % FAVO_ALIGNMENT) == 0);
        CHECK(favo_heap_usable_size(h, ptrs[i]) >= i);
    }
    CHECK_HEAP(h);
    for (size_t i = 0; i < 300; i += 2)
        favo_heap_free(h, ptrs[i]);
    CHECK_HEAP(h);
    for (size_t i = 1; i < 300; i += 2)
        favo_heap_free(h, ptrs[i]);
    CHECK_HEAP(h);
    favo_stats s;
    favo_heap_stats(h, &s);
    CHECK(s.used_blocks == 0);
    CHECK(s.free_blocks == s.arena_count);
    favo_heap_destroy(h);
}

static void calloc_zeroes_memory(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    unsigned char *a = favo_heap_alloc(h, 512);
    memset(a, 0xFF, 512);
    favo_heap_free(h, a);
    unsigned char *b = favo_heap_calloc(h, 64, 8);
    CHECK(b != NULL);
    int zero = 1;
    for (int i = 0; i < 512; i++)
        if (b[i])
            zero = 0;
    CHECK(zero);
    CHECK(favo_heap_calloc(h, SIZE_MAX / 2, 4) == NULL);
    CHECK(favo_heap_last_error(h) == FAVO_ENOMEM);
    favo_heap_destroy(h);
}

static void realloc_keeps_content(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    unsigned char *p = favo_heap_alloc(h, 40);
    for (int i = 0; i < 40; i++)
        p[i] = (unsigned char)i;
    unsigned char *q = favo_heap_realloc(h, p, 4000);
    CHECK(q != NULL);
    for (int i = 0; i < 40; i++)
        CHECK(q[i] == (unsigned char)i);
    unsigned char *r = favo_heap_realloc(h, q, 20);
    CHECK(r == q);
    for (int i = 0; i < 20; i++)
        CHECK(r[i] == (unsigned char)i);
    CHECK_HEAP(h);
    CHECK(favo_heap_realloc(h, r, 0) == NULL);
    CHECK_HEAP(h);
    void *n = favo_heap_realloc(h, NULL, 10);
    CHECK(n != NULL);
    favo_heap_free(h, n);
    favo_heap_destroy(h);
}

static void realloc_grows_in_place(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    void *a = favo_heap_alloc(h, 100);
    void *b = favo_heap_alloc(h, 400);
    void *c = favo_heap_alloc(h, 100);
    favo_heap_free(h, b);
    void *a2 = favo_heap_realloc(h, a, 300);
    CHECK(a2 == a);
    CHECK_HEAP(h);
    favo_heap_free(h, a2);
    favo_heap_free(h, c);
    CHECK_HEAP(h);
    favo_heap_destroy(h);
}

static void coalescing_merges_neighbours(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    void *a = favo_heap_alloc(h, 200);
    void *b = favo_heap_alloc(h, 200);
    void *c = favo_heap_alloc(h, 200);
    void *guard = favo_heap_alloc(h, 200);
    favo_stats s0;
    favo_heap_stats(h, &s0);
    favo_heap_free(h, a);
    favo_heap_free(h, c);
    favo_stats s1;
    favo_heap_stats(h, &s1);
    CHECK(s1.free_blocks == s0.free_blocks + 2);
    favo_heap_free(h, b);
    favo_stats s2;
    favo_heap_stats(h, &s2);
    CHECK(s2.free_blocks == s0.free_blocks + 1);
    CHECK(s2.coalesces >= 2);
    void *big = favo_heap_alloc(h, 600);
    CHECK(big == a);
    CHECK_HEAP(h);
    favo_heap_free(h, big);
    favo_heap_free(h, guard);
    favo_heap_destroy(h);
}

static void memalign_variants(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    size_t aligns[] = { 16, 32, 64, 128, 256, 1024, 4096, 65536 };
    void *ptrs[8];
    for (int i = 0; i < 8; i++) {
        ptrs[i] = favo_heap_memalign(h, aligns[i], 333);
        CHECK(ptrs[i] != NULL);
        CHECK(((uintptr_t)ptrs[i] % aligns[i]) == 0);
        memset(ptrs[i], 0x5A, 333);
        CHECK_HEAP(h);
    }
    for (int i = 0; i < 8; i++)
        CHECK(favo_heap_free(h, ptrs[i]) == FAVO_OK);
    CHECK_HEAP(h);
    CHECK(favo_heap_memalign(h, 48, 10) == NULL);
    favo_heap_destroy(h);
}

static void large_blocks_use_mmap(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    unsigned char *p = favo_heap_alloc(h, 1 << 20);
    CHECK(p != NULL);
    memset(p, 1, 1 << 20);
    favo_stats s;
    favo_heap_stats(h, &s);
    CHECK(s.large_blocks == 1);
    unsigned char *q = favo_heap_realloc(h, p, 2 << 20);
    CHECK(q != NULL);
    CHECK(q[12345] == 1);
    favo_heap_free(h, q);
    favo_heap_stats(h, &s);
    CHECK(s.large_blocks == 0);
    CHECK_HEAP(h);
    favo_heap_destroy(h);
}

static void errors_are_detected(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    void *p = favo_heap_alloc(h, 64);
    void *keep = favo_heap_alloc(h, 64);
    CHECK(favo_heap_free(h, p) == FAVO_OK);
    CHECK(favo_heap_free(h, p) == FAVO_EDOUBLE);
    int local = 0;
    CHECK(favo_heap_free(h, &local) != FAVO_OK);
    CHECK(favo_heap_free(h, (char *)keep + 16) != FAVO_OK);
    unsigned char *q = favo_heap_alloc(h, 64);
    memset(q, 0xEE, favo_heap_usable_size(h, q) + 8);
    CHECK(favo_heap_free(h, q) == FAVO_ECORRUPT);
    favo_heap_destroy(h);
}

static void budget_is_respected(void)
{
    favo_config cfg;
    favo_config_default(&cfg);
    cfg.source = FAVO_SOURCE_MMAP;
    cfg.grow_size = 64 * 1024;
    cfg.max_bytes = 128 * 1024;
    favo_heap *h = favo_heap_create(&cfg);
    int got = 0;
    for (int i = 0; i < 100; i++)
        if (favo_heap_alloc(h, 4000))
            got++;
    CHECK(got > 10 && got < 100);
    favo_stats s;
    favo_heap_stats(h, &s);
    CHECK(s.arena_bytes <= cfg.max_bytes);
    CHECK(s.failed_allocs > 0);
    CHECK_HEAP(h);
    favo_heap_destroy(h);
}

static void trim_and_reset(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    void *ptrs[100];
    for (int i = 0; i < 100; i++)
        ptrs[i] = favo_heap_alloc(h, 3000);
    favo_stats s;
    favo_heap_stats(h, &s);
    CHECK(s.arena_count > 1);
    for (int i = 0; i < 100; i++)
        favo_heap_free(h, ptrs[i]);
    favo_heap_trim(h, 0);
    favo_heap_stats(h, &s);
    CHECK(s.arena_count == 1);
    CHECK_HEAP(h);
    favo_heap_reset(h);
    favo_heap_stats(h, &s);
    CHECK(s.arena_count == 0 && s.total_allocs == 0);
    CHECK(favo_heap_alloc(h, 10) != NULL);
    CHECK_HEAP(h);
    favo_heap_destroy(h);
}

static void default_heap_api(void)
{
    char *p = favo_malloc(77);
    CHECK(p != NULL);
    strcpy(p, "favo");
    p = favo_realloc(p, 9000);
    CHECK(strcmp(p, "favo") == 0);
    void *a = NULL;
    CHECK(favo_posix_memalign(&a, 256, 100) == 0);
    CHECK(((uintptr_t)a % 256) == 0);
    CHECK(favo_usable_size(p) >= 9000);
    favo_free(a);
    favo_free(p);
    favo_free(NULL);
    CHECK(favo_aligned_alloc(3, 10) == NULL);
    CHECK_HEAP(favo_default_heap());
}

int main(void)
{
    RUN(alloc_and_free);
    RUN(alignment_of_every_size);
    RUN(calloc_zeroes_memory);
    RUN(realloc_keeps_content);
    RUN(realloc_grows_in_place);
    RUN(coalescing_merges_neighbours);
    RUN(memalign_variants);
    RUN(large_blocks_use_mmap);
    RUN(errors_are_detected);
    RUN(budget_is_respected);
    RUN(trim_and_reset);
    RUN(default_heap_api);
    return test_finish();
}
