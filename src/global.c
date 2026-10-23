#include "internal.h"

#include <errno.h>
#include <stdlib.h>

static favo_heap g_heap;
static volatile int g_ready;
static favo_lock g_init = FAVO_LOCK_INIT;

static int env_equals(const char *value, const char *word)
{
    while (*word) {
        char ch = *value;
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
        if (ch != *word)
            return 0;
        value++;
        word++;
    }
    return *value == '\0';
}

static size_t env_size(const char *value, size_t fallback)
{
    if (!value || !*value)
        return fallback;
    char *end = NULL;
    unsigned long long n = strtoull(value, &end, 10);
    if (end && (*end == 'k' || *end == 'K'))
        n <<= 10;
    else if (end && (*end == 'm' || *end == 'M'))
        n <<= 20;
    else if (end && (*end == 'g' || *end == 'G'))
        n <<= 30;
    return (size_t)n;
}

static void read_environment(favo_config *cfg)
{
    const char *v = getenv("FAVO_POLICY");
    if (v) {
        if (env_equals(v, "next") || env_equals(v, "next-fit"))
            cfg->policy = FAVO_NEXT_FIT;
        else if (env_equals(v, "best") || env_equals(v, "best-fit"))
            cfg->policy = FAVO_BEST_FIT;
        else
            cfg->policy = FAVO_FIRST_FIT;
    }
    v = getenv("FAVO_SOURCE");
    if (v)
        cfg->source = env_equals(v, "mmap") ? FAVO_SOURCE_MMAP : FAVO_SOURCE_SBRK;
    v = getenv("FAVO_ORDER");
    if (v)
        cfg->order = env_equals(v, "lifo") ? FAVO_ORDER_LIFO : FAVO_ORDER_ADDRESS;
    cfg->grow_size = env_size(getenv("FAVO_GROW"), cfg->grow_size);
    cfg->mmap_threshold = env_size(getenv("FAVO_MMAP_THRESHOLD"), cfg->mmap_threshold);
    cfg->max_bytes = env_size(getenv("FAVO_MAX_BYTES"), cfg->max_bytes);
    v = getenv("FAVO_ABORT");
    if (v)
        cfg->abort_on_error = !env_equals(v, "0");
}

favo_heap *favo_default_heap(void)
{
    if (!g_ready) {
        favo_lock_acquire(&g_init);
        if (!g_ready) {
            favo_config cfg;
            favo_config_default(&cfg);
            cfg.source = favo_os_has_sbrk() ? FAVO_SOURCE_SBRK : FAVO_SOURCE_MMAP;
            cfg.abort_on_error = 1;
            read_environment(&cfg);
            favo_heap_init_inplace(&g_heap, &cfg);
            g_ready = 1;
        }
        favo_lock_release(&g_init);
    }
    return &g_heap;
}

void *favo_malloc(size_t size)
{
    void *p = favo_heap_alloc(favo_default_heap(), size);
    if (!p)
        errno = ENOMEM;
    return p;
}

void favo_free(void *ptr)
{
    if (!ptr)
        return;
    favo_heap *h = favo_default_heap();
    if (!favo_heap_owns(h, ptr))
        return;
    favo_heap_free(h, ptr);
}

void *favo_calloc(size_t count, size_t size)
{
    void *p = favo_heap_calloc(favo_default_heap(), count, size);
    if (!p)
        errno = ENOMEM;
    return p;
}

void *favo_realloc(void *ptr, size_t size)
{
    favo_heap *h = favo_default_heap();
    if (ptr && !favo_heap_owns(h, ptr)) {
        errno = EINVAL;
        return NULL;
    }
    void *p = favo_heap_realloc(h, ptr, size);
    if (!p && size)
        errno = ENOMEM;
    return p;
}

void *favo_memalign(size_t alignment, size_t size)
{
    void *p = favo_heap_memalign(favo_default_heap(), alignment, size);
    if (!p)
        errno = ENOMEM;
    return p;
}

int favo_posix_memalign(void **out, size_t alignment, size_t size)
{
    if (!out || alignment < sizeof(void *) || (alignment & (alignment - 1)))
        return EINVAL;
    void *p = favo_heap_memalign(favo_default_heap(), alignment, size);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

void *favo_aligned_alloc(size_t alignment, size_t size)
{
    if (!alignment || (alignment & (alignment - 1))) {
        errno = EINVAL;
        return NULL;
    }
    return favo_memalign(alignment, size);
}

size_t favo_usable_size(const void *ptr)
{
    if (!ptr)
        return 0;
    favo_heap *h = favo_default_heap();
    if (!favo_heap_owns(h, ptr))
        return 0;
    return favo_heap_usable_size(h, ptr);
}

const char *favo_version(void)
{
    return FAVO_VERSION;
}

const char *favo_policy_name(int policy)
{
    switch (policy) {
    case FAVO_NEXT_FIT:
        return "next-fit";
    case FAVO_BEST_FIT:
        return "best-fit";
    default:
        return "first-fit";
    }
}

const char *favo_status_message(int status)
{
    switch (status) {
    case FAVO_OK:
        return "favo: ok";
    case FAVO_ENOMEM:
        return "favo: memoria insuficiente";
    case FAVO_EINVAL:
        return "favo: ponteiro ou argumento invalido";
    case FAVO_EDOUBLE:
        return "favo: double free detectado";
    case FAVO_EFOREIGN:
        return "favo: ponteiro nao pertence a este heap";
    case FAVO_ECORRUPT:
        return "favo: heap corrompido (escrita fora dos limites do bloco)";
    default:
        return "favo: erro desconhecido";
    }
}
