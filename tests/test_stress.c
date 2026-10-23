#include "test.h"

#include <stdint.h>

#define SLOTS 2048

static uint64_t state = 88172645463325252ull;

static uint64_t rnd(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static int verify(const unsigned char *p, size_t n, unsigned char tag)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != tag)
            return 0;
    return 1;
}

static void stress_policy(int policy, int order)
{
    favo_config cfg;
    favo_config_default(&cfg);
    cfg.policy = policy;
    cfg.order = order;
    cfg.source = FAVO_SOURCE_MMAP;
    cfg.grow_size = 128 * 1024;
    cfg.mmap_threshold = 64 * 1024;
    favo_heap *h = favo_heap_create(&cfg);
    static unsigned char *ptrs[SLOTS];
    static size_t sizes[SLOTS];
    memset(ptrs, 0, sizeof(ptrs));
    for (int i = 0; i < 150000; i++) {
        size_t s = (size_t)(rnd() % SLOTS);
        unsigned char tag = (unsigned char)(s * 31 + 7);
        if (ptrs[s]) {
            if (!verify(ptrs[s], sizes[s], tag)) {
                CHECK(!"conteudo corrompido");
                break;
            }
            if (rnd() % 3) {
                CHECK(favo_heap_free(h, ptrs[s]) == FAVO_OK);
                ptrs[s] = NULL;
            } else {
                size_t n = (size_t)(rnd() % 3000) + 1;
                unsigned char *q = favo_heap_realloc(h, ptrs[s], n);
                CHECK(q != NULL);
                size_t keep = n < sizes[s] ? n : sizes[s];
                CHECK(verify(q, keep, tag));
                memset(q, tag, n);
                ptrs[s] = q;
                sizes[s] = n;
            }
        } else {
            size_t n = rnd() % 50 == 0 ? (size_t)(rnd() % 200000) + 1 : (size_t)(rnd() % 2000) + 1;
            unsigned char *p;
            if (rnd() % 20 == 0)
                p = favo_heap_memalign(h, (size_t)1 << (5 + rnd() % 8), n);
            else
                p = favo_heap_alloc(h, n);
            CHECK(p != NULL);
            memset(p, tag, n);
            ptrs[s] = p;
            sizes[s] = n;
        }
        if (i % 5000 == 0)
            CHECK_HEAP(h);
    }
    CHECK_HEAP(h);
    for (int s = 0; s < SLOTS; s++)
        if (ptrs[s])
            favo_heap_free(h, ptrs[s]);
    CHECK_HEAP(h);
    favo_stats st;
    favo_heap_stats(h, &st);
    CHECK(st.used_blocks == 0);
    favo_heap_destroy(h);
}

static void stress_first_fit(void) { stress_policy(FAVO_FIRST_FIT, FAVO_ORDER_ADDRESS); }
static void stress_next_fit(void) { stress_policy(FAVO_NEXT_FIT, FAVO_ORDER_ADDRESS); }
static void stress_best_fit(void) { stress_policy(FAVO_BEST_FIT, FAVO_ORDER_ADDRESS); }
static void stress_lifo(void) { stress_policy(FAVO_FIRST_FIT, FAVO_ORDER_LIFO); }

static void stress_sbrk(void)
{
    favo_config cfg;
    favo_config_default(&cfg);
    cfg.source = FAVO_SOURCE_SBRK;
    cfg.policy = FAVO_BEST_FIT;
    cfg.grow_size = 64 * 1024;
    favo_heap *h = favo_heap_create(&cfg);
    void *ptrs[400];
    for (int i = 0; i < 400; i++) {
        ptrs[i] = favo_heap_alloc(h, (size_t)(i % 50) * 100 + 10);
        CHECK(ptrs[i] != NULL);
    }
    CHECK_HEAP(h);
    for (int i = 0; i < 400; i++)
        favo_heap_free(h, ptrs[i]);
    CHECK_HEAP(h);
    favo_heap_trim(h, 0);
    CHECK_HEAP(h);
    favo_heap_destroy(h);
}

int main(void)
{
    RUN(stress_first_fit);
    RUN(stress_next_fit);
    RUN(stress_best_fit);
    RUN(stress_lifo);
    RUN(stress_sbrk);
    return test_finish();
}
