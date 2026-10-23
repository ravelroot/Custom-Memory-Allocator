#ifndef FAVO_TEST_H
#define FAVO_TEST_H

#include "favo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: falhou: %s\n", __FILE__, __LINE__, #cond); \
            test_failures++; \
        } \
    } while (0)

#define CHECK_HEAP(h) \
    do { \
        char msg_[256]; \
        int rc_ = favo_heap_check((h), msg_, sizeof(msg_)); \
        if (rc_) { \
            fprintf(stderr, "%s:%d: heap inconsistente: %s\n", __FILE__, __LINE__, msg_); \
            test_failures++; \
        } \
    } while (0)

#define RUN(fn) \
    do { \
        int before_ = test_failures; \
        fn(); \
        printf("%-40s %s\n", #fn, test_failures == before_ ? "ok" : "FALHOU"); \
    } while (0)

static inline favo_heap *test_heap(int policy)
{
    favo_config cfg;
    favo_config_default(&cfg);
    cfg.policy = policy;
    cfg.source = FAVO_SOURCE_MMAP;
    cfg.grow_size = 64 * 1024;
    return favo_heap_create(&cfg);
}

static inline int test_finish(void)
{
    if (test_failures) {
        printf("%d falha(s)\n", test_failures);
        return 1;
    }
    printf("todos os testes passaram\n");
    return 0;
}

#endif
