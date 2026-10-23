#include "test.h"

typedef struct layout {
    favo_heap *h;
    void *big;
    void *small;
    void *mid;
} layout;

static layout prepare(int policy)
{
    layout l;
    l.h = test_heap(policy);
    void *g[8];
    l.big = favo_heap_alloc(l.h, 1000);
    g[0] = favo_heap_alloc(l.h, 64);
    l.small = favo_heap_alloc(l.h, 200);
    g[1] = favo_heap_alloc(l.h, 64);
    l.mid = favo_heap_alloc(l.h, 500);
    g[2] = favo_heap_alloc(l.h, 64);
    (void)g;
    favo_heap_free(l.h, l.big);
    favo_heap_free(l.h, l.small);
    favo_heap_free(l.h, l.mid);
    return l;
}

static void first_fit_takes_lowest_address(void)
{
    layout l = prepare(FAVO_FIRST_FIT);
    void *p = favo_heap_alloc(l.h, 150);
    CHECK(p == l.big);
    CHECK_HEAP(l.h);
    favo_heap_destroy(l.h);
}

static void best_fit_takes_smallest_block(void)
{
    layout l = prepare(FAVO_BEST_FIT);
    void *p = favo_heap_alloc(l.h, 150);
    CHECK(p == l.small);
    void *q = favo_heap_alloc(l.h, 450);
    CHECK(q == l.mid);
    CHECK_HEAP(l.h);
    favo_heap_destroy(l.h);
}

static void next_fit_continues_from_rover(void)
{
    layout l = prepare(FAVO_NEXT_FIT);
    favo_heap_set_policy(l.h, FAVO_NEXT_FIT);
    void *a = favo_heap_alloc(l.h, 150);
    void *b = favo_heap_alloc(l.h, 150);
    CHECK(a == l.big);
    CHECK(b != NULL);
    CHECK((char *)b > (char *)a);
    CHECK_HEAP(l.h);
    favo_heap_destroy(l.h);
}

static void switching_policy_keeps_heap_valid(void)
{
    favo_heap *h = test_heap(FAVO_FIRST_FIT);
    void *ptrs[64];
    for (int round = 0; round < 3; round++) {
        favo_heap_set_policy(h, round);
        CHECK(favo_heap_get_policy(h) == round);
        for (int i = 0; i < 64; i++)
            ptrs[i] = favo_heap_alloc(h, (size_t)(i * 37 % 900 + 1));
        for (int i = 0; i < 64; i += 3)
            favo_heap_free(h, ptrs[i]);
        CHECK_HEAP(h);
        for (int i = 0; i < 64; i++)
            if (i % 3)
                favo_heap_free(h, ptrs[i]);
        CHECK_HEAP(h);
    }
    CHECK(favo_heap_set_policy(h, 9) == FAVO_EINVAL);
    favo_heap_destroy(h);
}

static void lifo_order_is_consistent(void)
{
    favo_config cfg;
    favo_config_default(&cfg);
    cfg.order = FAVO_ORDER_LIFO;
    cfg.source = FAVO_SOURCE_MMAP;
    favo_heap *h = favo_heap_create(&cfg);
    void *ptrs[200];
    for (int i = 0; i < 200; i++)
        ptrs[i] = favo_heap_alloc(h, (size_t)(i % 13) * 40 + 8);
    for (int i = 0; i < 200; i += 2)
        favo_heap_free(h, ptrs[i]);
    CHECK_HEAP(h);
    for (int i = 1; i < 200; i += 2)
        favo_heap_free(h, ptrs[i]);
    CHECK_HEAP(h);
    favo_heap_destroy(h);
}

static void inspection_matches_stats(void)
{
    favo_heap *h = test_heap(FAVO_BEST_FIT);
    void *ptrs[50];
    for (int i = 0; i < 50; i++)
        ptrs[i] = favo_heap_alloc(h, (size_t)(i + 1) * 24);
    for (int i = 0; i < 50; i += 4)
        favo_heap_free(h, ptrs[i]);
    favo_stats s;
    favo_heap_stats(h, &s);
    size_t blocks = favo_heap_snapshot(h, NULL, 0);
    CHECK(blocks == s.used_blocks + s.free_blocks);
    CHECK(favo_heap_free_list(h, NULL, 0) == s.free_blocks);
    CHECK(favo_heap_tree(h, NULL, 0) == s.free_blocks);
    favo_tree_node nodes[64];
    size_t n = favo_heap_tree(h, nodes, 64);
    CHECK(n > 0 && nodes[0].red == 0 && nodes[0].depth == 0);
    favo_arena_info arenas[4];
    CHECK(favo_heap_arenas(h, arenas, 4) == s.arena_count);
    favo_heap_destroy(h);
}

static void benchmark_runs(void)
{
    favo_bench_result r;
    for (int p = 0; p < 3; p++) {
        CHECK(favo_bench_run(p, 20000, 7, 16, 2048, &r) == FAVO_OK);
        CHECK(r.operations == 20000 && r.failed == 0 && r.ops_per_second > 0);
    }
    CHECK(favo_bench_system(20000, 7, 16, 2048, &r) == FAVO_OK);
    CHECK(favo_bench_run(0, 10, 1, 0, 10, &r) == FAVO_EINVAL);
}

int main(void)
{
    RUN(first_fit_takes_lowest_address);
    RUN(best_fit_takes_smallest_block);
    RUN(next_fit_continues_from_rover);
    RUN(switching_policy_keeps_heap_valid);
    RUN(lifo_order_is_consistent);
    RUN(inspection_matches_stats);
    RUN(benchmark_runs);
    return test_finish();
}
