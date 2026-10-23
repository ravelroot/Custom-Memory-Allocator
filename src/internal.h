#ifndef FAVO_INTERNAL_H
#define FAVO_INTERNAL_H

#include "favo.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef SRWLOCK favo_lock;
#  define FAVO_LOCK_INIT SRWLOCK_INIT
#else
#  include <pthread.h>
typedef pthread_mutex_t favo_lock;
#  define FAVO_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#endif

#define FAVO_FLAG_USED ((size_t)1)
#define FAVO_FLAG_LARGE ((size_t)2)
#define FAVO_FLAG_MASK ((size_t)15)

#define FAVO_MAGIC_USED 0xFA70B10Cu
#define FAVO_MAGIC_FREE 0xFA70F2EEu
#define FAVO_MAGIC_EDGE 0xFA70ED6Eu

#define FAVO_ARENA_HEAD 64
#define FAVO_ARENA_TAIL 16
#define FAVO_ARENA_OVERHEAD (FAVO_ARENA_HEAD + FAVO_ARENA_TAIL)
#define FAVO_OVERHEAD (FAVO_HEADER_SIZE + FAVO_FOOTER_SIZE)

#define FAVO_DEFAULT_GROW ((size_t)1 << 20)
#define FAVO_DEFAULT_MMAP_THRESHOLD ((size_t)256 << 10)

typedef struct favo_block {
    size_t info;
    uint32_t magic;
    uint32_t request;
} favo_block;

typedef struct favo_node {
    favo_block head;
    struct favo_node *next;
    struct favo_node *prev;
    struct favo_node *left;
    struct favo_node *right;
    uintptr_t parent_color;
} favo_node;

typedef struct favo_arena {
    struct favo_arena *next;
    unsigned char *base;
    size_t length;
    unsigned char *first;
    unsigned char *end;
    int source;
    int large;
} favo_arena;

struct favo_heap {
    favo_lock lock;
    favo_config cfg;
    favo_arena *arenas;
    favo_node *list;
    favo_node *rover;
    favo_node *root;
    size_t arena_bytes;
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
    int last_error;
    int owned_struct;
    int sbrk_ready;
};

static inline size_t favo_align_up(size_t value, size_t align)
{
    return (value + align - 1) & ~(align - 1);
}

static inline size_t blk_size(const favo_block *b)
{
    return b->info & ~FAVO_FLAG_MASK;
}

static inline int blk_used(const favo_block *b)
{
    return (int)(b->info & FAVO_FLAG_USED);
}

static inline int blk_large(const favo_block *b)
{
    return (b->info & FAVO_FLAG_LARGE) != 0;
}

static inline size_t *blk_footer(favo_block *b)
{
    return (size_t *)((unsigned char *)b + blk_size(b) - FAVO_FOOTER_SIZE);
}

static inline favo_block *blk_next(favo_block *b)
{
    return (favo_block *)((unsigned char *)b + blk_size(b));
}

static inline size_t blk_prev_info(favo_block *b)
{
    return *((size_t *)b - 1);
}

static inline favo_block *blk_prev(favo_block *b)
{
    return (favo_block *)((unsigned char *)b - (blk_prev_info(b) & ~FAVO_FLAG_MASK));
}

static inline void *blk_payload(favo_block *b)
{
    return (unsigned char *)b + FAVO_HEADER_SIZE;
}

static inline favo_block *blk_from_payload(const void *p)
{
    return (favo_block *)((unsigned char *)p - FAVO_HEADER_SIZE);
}

static inline void blk_set(favo_block *b, size_t size, size_t flags, uint32_t magic)
{
    b->info = size | flags;
    b->magic = magic;
    *blk_footer(b) = b->info;
}

void favo_lock_init(favo_lock *lock);
void favo_lock_destroy(favo_lock *lock);
void favo_lock_acquire(favo_lock *lock);
void favo_lock_release(favo_lock *lock);

size_t favo_os_page(void);
void *favo_os_map(size_t length);
int favo_os_unmap(void *base, size_t length);
int favo_os_has_sbrk(void);
void *favo_os_sbrk(intptr_t increment);
void favo_os_fatal(const char *message);

void favo_list_insert(favo_heap *h, favo_node *f);
void favo_list_remove(favo_heap *h, favo_node *f);
void favo_list_replace(favo_heap *h, favo_node *old, favo_node *repl);

void favo_tree_insert(favo_heap *h, favo_node *f);
void favo_tree_remove(favo_heap *h, favo_node *f);
favo_node *favo_tree_lower_bound(favo_heap *h, size_t size, uint64_t *steps);
favo_node *favo_tree_parent(const favo_node *f);
int favo_tree_is_red(const favo_node *f);

favo_arena *favo_arena_of(favo_heap *h, const void *ptr);
void favo_heap_init_inplace(favo_heap *h, const favo_config *cfg);

#endif
