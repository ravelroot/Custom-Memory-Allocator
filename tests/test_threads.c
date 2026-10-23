#include "test.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define THREADS 8
#define ROUNDS 20000

static favo_heap *shared;
static volatile int thread_errors;

static void work(unsigned seed)
{
    void *ptrs[64] = { 0 };
    unsigned x = seed * 2654435761u + 1;
    for (int i = 0; i < ROUNDS; i++) {
        x = x * 1103515245u + 12345u;
        int s = (int)((x >> 8) % 64);
        if (ptrs[s]) {
            if (*(unsigned char *)ptrs[s] != (unsigned char)s)
                thread_errors++;
            favo_heap_free(shared, ptrs[s]);
            ptrs[s] = NULL;
        } else {
            size_t n = (size_t)((x >> 4) % 1500) + 1;
            ptrs[s] = favo_heap_alloc(shared, n);
            if (!ptrs[s])
                thread_errors++;
            else
                memset(ptrs[s], s, n);
        }
    }
    for (int s = 0; s < 64; s++)
        if (ptrs[s])
            favo_heap_free(shared, ptrs[s]);
}

#ifdef _WIN32
static DWORD WINAPI entry(LPVOID arg)
{
    work((unsigned)(uintptr_t)arg);
    return 0;
}
#else
static void *entry(void *arg)
{
    work((unsigned)(uintptr_t)arg);
    return NULL;
}
#endif

static void concurrent_alloc_free(void)
{
    for (int policy = 0; policy < 3; policy++) {
        shared = test_heap(policy);
        thread_errors = 0;
#ifdef _WIN32
        HANDLE t[THREADS];
        for (int i = 0; i < THREADS; i++)
            t[i] = CreateThread(NULL, 0, entry, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
        WaitForMultipleObjects(THREADS, t, TRUE, INFINITE);
#else
        pthread_t t[THREADS];
        for (int i = 0; i < THREADS; i++)
            pthread_create(&t[i], NULL, entry, (void *)(uintptr_t)(i + 1));
        for (int i = 0; i < THREADS; i++)
            pthread_join(t[i], NULL);
#endif
        CHECK(thread_errors == 0);
        CHECK_HEAP(shared);
        favo_stats s;
        favo_heap_stats(shared, &s);
        CHECK(s.used_blocks == 0);
        favo_heap_destroy(shared);
    }
}

int main(void)
{
    RUN(concurrent_alloc_free);
    return test_finish();
}
