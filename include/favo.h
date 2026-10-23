#ifndef FAVO_H
#define FAVO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(FAVO_SHARED)
#  ifdef FAVO_BUILDING
#    define FAVO_API __declspec(dllexport)
#  else
#    define FAVO_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define FAVO_API __attribute__((visibility("default")))
#else
#  define FAVO_API
#endif

#define FAVO_VERSION_MAJOR 1
#define FAVO_VERSION_MINOR 0
#define FAVO_VERSION_PATCH 0
#define FAVO_VERSION "1.0.0"

#define FAVO_ALIGNMENT 16
#define FAVO_HEADER_SIZE 16
#define FAVO_FOOTER_SIZE 8
#define FAVO_MIN_BLOCK 64

typedef enum favo_policy {
    FAVO_FIRST_FIT = 0,
    FAVO_NEXT_FIT = 1,
    FAVO_BEST_FIT = 2
} favo_policy;

typedef enum favo_source {
    FAVO_SOURCE_AUTO = 0,
    FAVO_SOURCE_SBRK = 1,
    FAVO_SOURCE_MMAP = 2
} favo_source;

typedef enum favo_order {
    FAVO_ORDER_ADDRESS = 0,
    FAVO_ORDER_LIFO = 1
} favo_order;

typedef enum favo_status {
    FAVO_OK = 0,
    FAVO_ENOMEM = 1,
    FAVO_EINVAL = 2,
    FAVO_EDOUBLE = 3,
    FAVO_EFOREIGN = 4,
    FAVO_ECORRUPT = 5
} favo_status;

typedef struct favo_config {
    int policy;
    int source;
    int order;
    int thread_safe;
    int abort_on_error;
    int release_empty;
    size_t grow_size;
    size_t mmap_threshold;
    size_t max_bytes;
} favo_config;

typedef struct favo_stats {
    size_t arena_count;
    size_t arena_bytes;
    size_t used_blocks;
    size_t free_blocks;
    size_t used_bytes;
    size_t free_bytes;
    size_t requested_bytes;
    size_t largest_free;
    size_t large_blocks;
    size_t large_bytes;
    size_t peak_arena_bytes;
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t total_reallocs;
    uint64_t failed_allocs;
    uint64_t splits;
    uint64_t coalesces;
    uint64_t os_requests;
    uint64_t os_releases;
    uint64_t search_steps;
    double fragmentation;
    double utilization;
} favo_stats;

typedef struct favo_block_info {
    size_t arena;
    size_t offset;
    size_t size;
    size_t payload;
    size_t request;
    int used;
    int large;
    uintptr_t address;
} favo_block_info;

typedef struct favo_arena_info {
    size_t index;
    uintptr_t base;
    size_t length;
    size_t first_offset;
    size_t end_offset;
    int source;
    int large;
} favo_arena_info;

typedef struct favo_tree_node {
    uintptr_t address;
    uintptr_t left;
    uintptr_t right;
    size_t size;
    int red;
    int depth;
} favo_tree_node;

typedef struct favo_bench_result {
    int policy;
    uint64_t operations;
    double seconds;
    double ops_per_second;
    size_t peak_arena_bytes;
    double fragmentation;
    double utilization;
    uint64_t search_steps;
    uint64_t failed;
} favo_bench_result;

typedef struct favo_heap favo_heap;

FAVO_API void favo_config_default(favo_config *cfg);
FAVO_API favo_heap *favo_heap_create(const favo_config *cfg);
FAVO_API void favo_heap_destroy(favo_heap *heap);
FAVO_API void favo_heap_reset(favo_heap *heap);

FAVO_API void *favo_heap_alloc(favo_heap *heap, size_t size);
FAVO_API void *favo_heap_calloc(favo_heap *heap, size_t count, size_t size);
FAVO_API void *favo_heap_realloc(favo_heap *heap, void *ptr, size_t size);
FAVO_API void *favo_heap_memalign(favo_heap *heap, size_t alignment, size_t size);
FAVO_API int favo_heap_free(favo_heap *heap, void *ptr);
FAVO_API size_t favo_heap_usable_size(favo_heap *heap, const void *ptr);
FAVO_API int favo_heap_owns(favo_heap *heap, const void *ptr);

FAVO_API int favo_heap_set_policy(favo_heap *heap, int policy);
FAVO_API int favo_heap_get_policy(favo_heap *heap);
FAVO_API size_t favo_heap_trim(favo_heap *heap, size_t pad);
FAVO_API void favo_heap_stats(favo_heap *heap, favo_stats *out);
FAVO_API int favo_heap_check(favo_heap *heap, char *message, size_t length);
FAVO_API size_t favo_heap_snapshot(favo_heap *heap, favo_block_info *out, size_t max);
FAVO_API size_t favo_heap_arenas(favo_heap *heap, favo_arena_info *out, size_t max);
FAVO_API size_t favo_heap_free_list(favo_heap *heap, uintptr_t *out, size_t max);
FAVO_API size_t favo_heap_tree(favo_heap *heap, favo_tree_node *out, size_t max);
FAVO_API int favo_heap_last_error(favo_heap *heap);

FAVO_API favo_heap *favo_default_heap(void);
FAVO_API void *favo_malloc(size_t size);
FAVO_API void favo_free(void *ptr);
FAVO_API void *favo_calloc(size_t count, size_t size);
FAVO_API void *favo_realloc(void *ptr, size_t size);
FAVO_API void *favo_memalign(size_t alignment, size_t size);
FAVO_API int favo_posix_memalign(void **out, size_t alignment, size_t size);
FAVO_API void *favo_aligned_alloc(size_t alignment, size_t size);
FAVO_API size_t favo_usable_size(const void *ptr);

FAVO_API int favo_bench_run(int policy, uint64_t operations, uint64_t seed, size_t min_size, size_t max_size, favo_bench_result *out);
FAVO_API int favo_bench_system(uint64_t operations, uint64_t seed, size_t min_size, size_t max_size, favo_bench_result *out);

FAVO_API const char *favo_version(void);
FAVO_API const char *favo_policy_name(int policy);
FAVO_API const char *favo_status_message(int status);
FAVO_API size_t favo_page_size(void);

#ifdef __cplusplus
}
#endif

#endif
