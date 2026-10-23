#include "internal.h"

static uint32_t clamp_request(size_t size)
{
    return size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
}

static int fail(favo_heap *h, int code)
{
    h->last_error = code;
    if (h->cfg.abort_on_error && code != FAVO_ENOMEM)
        favo_os_fatal(favo_status_message(code));
    return code;
}

static int block_size_for(size_t request, size_t *out)
{
    if (request > SIZE_MAX / 2)
        return 0;
    size_t n = favo_align_up((request ? request : 1) + FAVO_OVERHEAD, FAVO_ALIGNMENT);
    if (n < FAVO_MIN_BLOCK)
        n = FAVO_MIN_BLOCK;
    *out = n;
    return 1;
}

void favo_config_default(favo_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->policy = FAVO_FIRST_FIT;
    cfg->source = FAVO_SOURCE_AUTO;
    cfg->order = FAVO_ORDER_ADDRESS;
    cfg->thread_safe = 1;
    cfg->abort_on_error = 0;
    cfg->release_empty = 1;
    cfg->grow_size = FAVO_DEFAULT_GROW;
    cfg->mmap_threshold = FAVO_DEFAULT_MMAP_THRESHOLD;
    cfg->max_bytes = 0;
}

static void normalize(favo_config *c)
{
    size_t page = favo_os_page();
    if (c->policy < FAVO_FIRST_FIT || c->policy > FAVO_BEST_FIT)
        c->policy = FAVO_FIRST_FIT;
    if (c->order != FAVO_ORDER_LIFO)
        c->order = FAVO_ORDER_ADDRESS;
    if (c->source != FAVO_SOURCE_SBRK && c->source != FAVO_SOURCE_MMAP)
        c->source = FAVO_SOURCE_MMAP;
    if (c->source == FAVO_SOURCE_SBRK && !favo_os_has_sbrk())
        c->source = FAVO_SOURCE_MMAP;
    if (c->grow_size < 4 * page)
        c->grow_size = 4 * page;
    c->grow_size = favo_align_up(c->grow_size, page);
    if (c->mmap_threshold && c->mmap_threshold < FAVO_MIN_BLOCK * 4)
        c->mmap_threshold = FAVO_MIN_BLOCK * 4;
}

void favo_heap_init_inplace(favo_heap *h, const favo_config *cfg)
{
    memset(h, 0, sizeof(*h));
    if (cfg)
        h->cfg = *cfg;
    else
        favo_config_default(&h->cfg);
    normalize(&h->cfg);
    favo_lock_init(&h->lock);
}

static void lock(favo_heap *h)
{
    if (h->cfg.thread_safe)
        favo_lock_acquire(&h->lock);
}

static void unlock(favo_heap *h)
{
    if (h->cfg.thread_safe)
        favo_lock_release(&h->lock);
}

static favo_arena *arena_init(unsigned char *base, size_t length, int source, int large)
{
    favo_arena *a = (favo_arena *)base;
    a->next = NULL;
    a->base = base;
    a->length = length;
    a->first = base + FAVO_ARENA_HEAD;
    a->end = base + length - FAVO_ARENA_TAIL;
    a->source = source;
    a->large = large;
    *((size_t *)a->first - 1) = FAVO_FLAG_USED;
    favo_block *e = (favo_block *)a->end;
    e->info = FAVO_FLAG_USED;
    e->magic = FAVO_MAGIC_EDGE;
    e->request = 0;
    return a;
}

static void arena_link(favo_heap *h, favo_arena *a)
{
    favo_arena **slot = &h->arenas;
    while (*slot)
        slot = &(*slot)->next;
    *slot = a;
    h->arena_bytes += a->length;
    if (h->arena_bytes > h->peak_arena_bytes)
        h->peak_arena_bytes = h->arena_bytes;
    h->os_requests++;
}

static void arena_unlink(favo_heap *h, favo_arena *a)
{
    favo_arena **slot = &h->arenas;
    while (*slot && *slot != a)
        slot = &(*slot)->next;
    if (*slot)
        *slot = a->next;
    h->arena_bytes -= a->length;
}

static void arena_release(favo_heap *h, favo_arena *a)
{
    arena_unlink(h, a);
    h->os_releases++;
    if (a->source == FAVO_SOURCE_SBRK) {
        unsigned char *brk = favo_os_sbrk(0);
        if (brk == a->base + a->length)
            favo_os_sbrk(-(intptr_t)a->length);
        return;
    }
    favo_os_unmap(a->base, a->length);
}

favo_arena *favo_arena_of(favo_heap *h, const void *ptr)
{
    const unsigned char *p = (const unsigned char *)ptr;
    for (favo_arena *a = h->arenas; a; a = a->next) {
        if (p >= a->first && p < a->end)
            return a;
    }
    return NULL;
}

static size_t regular_arenas(favo_heap *h)
{
    size_t n = 0;
    for (favo_arena *a = h->arenas; a; a = a->next)
        if (!a->large)
            n++;
    return n;
}

static int within_budget(favo_heap *h, size_t len)
{
    return !h->cfg.max_bytes || h->arena_bytes + len <= h->cfg.max_bytes;
}

static favo_node *add_free_region(favo_heap *h, favo_arena *a)
{
    favo_block *b = (favo_block *)a->first;
    blk_set(b, (size_t)(a->end - a->first), 0, FAVO_MAGIC_FREE);
    b->request = 0;
    favo_node *f = (favo_node *)b;
    favo_list_insert(h, f);
    favo_tree_insert(h, f);
    return f;
}

static favo_node *grow_sbrk(favo_heap *h, size_t need, size_t len)
{
    size_t page = favo_os_page();
    unsigned char *brk0 = favo_os_sbrk(0);
    if (brk0 == (unsigned char *)-1)
        return NULL;
    favo_arena *top = NULL;
    for (favo_arena *a = h->arenas; a; a = a->next)
        if (a->source == FAVO_SOURCE_SBRK && a->base + a->length == brk0)
            top = a;
    if (top) {
        size_t inc = favo_align_up(need > h->cfg.grow_size ? need : h->cfg.grow_size, page);
        if (!within_budget(h, inc)) {
            inc = favo_align_up(need, page);
            if (!within_budget(h, inc))
                return NULL;
        }
        if ((unsigned char *)favo_os_sbrk((intptr_t)inc) != brk0)
            return NULL;
        favo_block *nb = (favo_block *)top->end;
        top->length += inc;
        top->end += inc;
        favo_block *e = (favo_block *)top->end;
        e->info = FAVO_FLAG_USED;
        e->magic = FAVO_MAGIC_EDGE;
        e->request = 0;
        h->arena_bytes += inc;
        if (h->arena_bytes > h->peak_arena_bytes)
            h->peak_arena_bytes = h->arena_bytes;
        h->os_requests++;
        if (!(blk_prev_info(nb) & FAVO_FLAG_USED)) {
            favo_node *p = (favo_node *)blk_prev(nb);
            favo_tree_remove(h, p);
            blk_set(&p->head, blk_size(&p->head) + inc, 0, FAVO_MAGIC_FREE);
            favo_tree_insert(h, p);
            h->coalesces++;
            return p;
        }
        blk_set(nb, inc, 0, FAVO_MAGIC_FREE);
        nb->request = 0;
        favo_node *f = (favo_node *)nb;
        favo_list_insert(h, f);
        favo_tree_insert(h, f);
        return f;
    }
    size_t pad = favo_align_up((uintptr_t)brk0, FAVO_ALIGNMENT) - (uintptr_t)brk0;
    unsigned char *old = favo_os_sbrk((intptr_t)(len + pad));
    if (old == (unsigned char *)-1)
        return NULL;
    unsigned char *base = (unsigned char *)favo_align_up((uintptr_t)old, FAVO_ALIGNMENT);
    size_t length = (size_t)(old + len + pad - base) & ~(size_t)(FAVO_ALIGNMENT - 1);
    favo_arena *a = arena_init(base, length, FAVO_SOURCE_SBRK, 0);
    arena_link(h, a);
    return add_free_region(h, a);
}

static favo_node *grow(favo_heap *h, size_t need)
{
    size_t page = favo_os_page();
    size_t want = need + FAVO_ARENA_OVERHEAD;
    size_t len = favo_align_up(want > h->cfg.grow_size ? want : h->cfg.grow_size, page);
    if (!within_budget(h, len)) {
        len = favo_align_up(want, page);
        if (!within_budget(h, len))
            return NULL;
    }
    if (h->cfg.source == FAVO_SOURCE_SBRK) {
        favo_node *f = grow_sbrk(h, need, len);
        if (f)
            return f;
    }
    unsigned char *base = favo_os_map(len);
    if (!base)
        return NULL;
    favo_arena *a = arena_init(base, len, FAVO_SOURCE_MMAP, 0);
    arena_link(h, a);
    return add_free_region(h, a);
}

static void *alloc_large(favo_heap *h, size_t need, size_t align, size_t request)
{
    size_t page = favo_os_page();
    size_t extra = align > FAVO_ALIGNMENT ? align : 0;
    if (need > SIZE_MAX / 2 - extra)
        return NULL;
    size_t len = favo_align_up(need + FAVO_ARENA_OVERHEAD + extra, page);
    if (!within_budget(h, len))
        return NULL;
    unsigned char *base = favo_os_map(len);
    if (!base)
        return NULL;
    favo_arena *a = arena_init(base, len, FAVO_SOURCE_MMAP, 1);
    if (extra) {
        uintptr_t payload = favo_align_up((uintptr_t)(base + FAVO_ARENA_HEAD + FAVO_HEADER_SIZE), align);
        a->first = (unsigned char *)(payload - FAVO_HEADER_SIZE);
        *((size_t *)a->first - 1) = FAVO_FLAG_USED;
    }
    arena_link(h, a);
    favo_block *b = (favo_block *)a->first;
    blk_set(b, (size_t)(a->end - a->first), FAVO_FLAG_USED | FAVO_FLAG_LARGE, FAVO_MAGIC_USED);
    b->request = clamp_request(request);
    return blk_payload(b);
}

static favo_node *find_fit(favo_heap *h, size_t need)
{
    if (h->cfg.policy == FAVO_BEST_FIT)
        return favo_tree_lower_bound(h, need, &h->search_steps);
    favo_node *start = h->list;
    if (h->cfg.policy == FAVO_NEXT_FIT && h->rover)
        start = h->rover;
    if (!start)
        return NULL;
    favo_node *f = start;
    do {
        h->search_steps++;
        if (blk_size(&f->head) >= need) {
            if (h->cfg.policy == FAVO_NEXT_FIT)
                h->rover = f;
            return f;
        }
        f = f->next;
    } while (f != start);
    return NULL;
}

static void *place(favo_heap *h, favo_node *f, size_t need, size_t request)
{
    size_t total = blk_size(&f->head);
    favo_tree_remove(h, f);
    if (total - need >= FAVO_MIN_BLOCK) {
        favo_node *r = (favo_node *)((unsigned char *)f + need);
        favo_list_replace(h, f, r);
        blk_set(&r->head, total - need, 0, FAVO_MAGIC_FREE);
        r->head.request = 0;
        favo_tree_insert(h, r);
        h->splits++;
    } else {
        favo_list_remove(h, f);
        need = total;
    }
    favo_block *b = &f->head;
    blk_set(b, need, FAVO_FLAG_USED, FAVO_MAGIC_USED);
    b->request = clamp_request(request);
    return blk_payload(b);
}

static void *alloc_unlocked(favo_heap *h, size_t size)
{
    size_t need;
    if (!block_size_for(size, &need)) {
        h->failed_allocs++;
        fail(h, FAVO_ENOMEM);
        return NULL;
    }
    void *p;
    if (h->cfg.mmap_threshold && need >= h->cfg.mmap_threshold) {
        p = alloc_large(h, need, 0, size);
    } else {
        favo_node *f = find_fit(h, need);
        if (!f) {
            f = grow(h, need);
            if (f && h->cfg.policy == FAVO_NEXT_FIT)
                h->rover = f;
        }
        p = f ? place(h, f, need, size) : NULL;
    }
    if (!p) {
        h->failed_allocs++;
        fail(h, FAVO_ENOMEM);
        return NULL;
    }
    h->total_allocs++;
    h->last_error = FAVO_OK;
    return p;
}

static favo_node *coalesce_insert(favo_heap *h, favo_block *b)
{
    size_t size = blk_size(b);
    int prev_free = !(blk_prev_info(b) & FAVO_FLAG_USED);
    favo_block *n = blk_next(b);
    int next_free = !blk_used(n);
    b->info &= ~FAVO_FLAG_USED;
    b->magic = FAVO_MAGIC_FREE;
    b->request = 0;
    if (prev_free && next_free) {
        favo_node *p = (favo_node *)blk_prev(b);
        favo_node *nf = (favo_node *)n;
        size_t nsize = blk_size(n);
        favo_tree_remove(h, p);
        favo_tree_remove(h, nf);
        favo_list_remove(h, nf);
        blk_set(&p->head, blk_size(&p->head) + size + nsize, 0, FAVO_MAGIC_FREE);
        favo_tree_insert(h, p);
        h->coalesces += 2;
        return p;
    }
    if (prev_free) {
        favo_node *p = (favo_node *)blk_prev(b);
        favo_tree_remove(h, p);
        blk_set(&p->head, blk_size(&p->head) + size, 0, FAVO_MAGIC_FREE);
        favo_tree_insert(h, p);
        h->coalesces++;
        return p;
    }
    if (next_free) {
        favo_node *nf = (favo_node *)n;
        size_t nsize = blk_size(n);
        favo_tree_remove(h, nf);
        favo_list_replace(h, nf, (favo_node *)b);
        blk_set(b, size + nsize, 0, FAVO_MAGIC_FREE);
        favo_tree_insert(h, (favo_node *)b);
        h->coalesces++;
        return (favo_node *)b;
    }
    blk_set(b, size, 0, FAVO_MAGIC_FREE);
    favo_list_insert(h, (favo_node *)b);
    favo_tree_insert(h, (favo_node *)b);
    return (favo_node *)b;
}

static size_t trim_arena(favo_heap *h, favo_arena *a, size_t pad)
{
    if (a->large)
        return 0;
    favo_block *edge = (favo_block *)a->end;
    if (blk_prev_info(edge) & FAVO_FLAG_USED)
        return 0;
    favo_node *last = (favo_node *)blk_prev(edge);
    size_t lsize = blk_size(&last->head);
    if (a->source == FAVO_SOURCE_MMAP) {
        if ((unsigned char *)last == a->first && regular_arenas(h) > 1) {
            favo_tree_remove(h, last);
            favo_list_remove(h, last);
            size_t len = a->length;
            arena_release(h, a);
            return len;
        }
        return 0;
    }
    unsigned char *brk = favo_os_sbrk(0);
    if (brk != a->base + a->length)
        return 0;
    size_t page = favo_os_page();
    if (lsize <= pad + FAVO_MIN_BLOCK + page)
        return 0;
    size_t k = (lsize - pad - FAVO_MIN_BLOCK) & ~(page - 1);
    if (!k)
        return 0;
    favo_tree_remove(h, last);
    blk_set(&last->head, lsize - k, 0, FAVO_MAGIC_FREE);
    favo_tree_insert(h, last);
    a->length -= k;
    a->end -= k;
    favo_block *e = (favo_block *)a->end;
    e->info = FAVO_FLAG_USED;
    e->magic = FAVO_MAGIC_EDGE;
    e->request = 0;
    favo_os_sbrk(-(intptr_t)k);
    h->arena_bytes -= k;
    h->os_releases++;
    return k;
}

static void release_block(favo_heap *h, favo_arena *a, favo_block *b)
{
    favo_node *f = coalesce_insert(h, b);
    if (!h->cfg.release_empty)
        return;
    favo_block *n = blk_next(&f->head);
    if ((unsigned char *)n != a->end)
        return;
    if (a->source == FAVO_SOURCE_MMAP && (unsigned char *)f == a->first)
        trim_arena(h, a, 0);
    else if (a->source == FAVO_SOURCE_SBRK && blk_size(&f->head) >= 2 * h->cfg.grow_size)
        trim_arena(h, a, h->cfg.grow_size);
}

static favo_block *validate(favo_heap *h, const void *ptr, favo_arena **out)
{
    if ((uintptr_t)ptr & (FAVO_ALIGNMENT - 1)) {
        fail(h, FAVO_EINVAL);
        return NULL;
    }
    favo_arena *a = favo_arena_of(h, ptr);
    if (!a) {
        fail(h, FAVO_EFOREIGN);
        return NULL;
    }
    favo_block *b = blk_from_payload(ptr);
    if ((unsigned char *)b < a->first || (unsigned char *)b >= a->end) {
        fail(h, FAVO_EINVAL);
        return NULL;
    }
    if (b->magic == FAVO_MAGIC_FREE && !blk_used(b)) {
        fail(h, FAVO_EDOUBLE);
        return NULL;
    }
    if (b->magic != FAVO_MAGIC_USED || !blk_used(b)) {
        fail(h, FAVO_EINVAL);
        return NULL;
    }
    size_t size = blk_size(b);
    if (size < FAVO_MIN_BLOCK || size > (size_t)(a->end - (unsigned char *)b) || *blk_footer(b) != b->info) {
        fail(h, FAVO_ECORRUPT);
        return NULL;
    }
    if (out)
        *out = a;
    return b;
}

static int free_unlocked(favo_heap *h, void *ptr)
{
    if (!ptr)
        return FAVO_OK;
    favo_arena *a = NULL;
    favo_block *b = validate(h, ptr, &a);
    if (!b)
        return h->last_error;
    h->total_frees++;
    if (blk_large(b)) {
        b->magic = FAVO_MAGIC_FREE;
        arena_release(h, a);
    } else {
        release_block(h, a, b);
    }
    h->last_error = FAVO_OK;
    return FAVO_OK;
}

static void shrink_used(favo_heap *h, favo_block *b, size_t need)
{
    size_t cur = blk_size(b);
    if (cur < need + FAVO_MIN_BLOCK)
        return;
    uint32_t request = b->request;
    favo_block *t = (favo_block *)((unsigned char *)b + need);
    blk_set(b, need, FAVO_FLAG_USED, FAVO_MAGIC_USED);
    b->request = request;
    blk_set(t, cur - need, FAVO_FLAG_USED, FAVO_MAGIC_USED);
    coalesce_insert(h, t);
    h->splits++;
}

static void *realloc_unlocked(favo_heap *h, void *ptr, size_t size)
{
    if (!ptr)
        return alloc_unlocked(h, size);
    if (size == 0) {
        free_unlocked(h, ptr);
        return NULL;
    }
    favo_arena *a = NULL;
    favo_block *b = validate(h, ptr, &a);
    if (!b)
        return NULL;
    size_t need;
    if (!block_size_for(size, &need)) {
        fail(h, FAVO_ENOMEM);
        return NULL;
    }
    h->total_reallocs++;
    size_t cur = blk_size(b);
    if (!blk_large(b)) {
        if (need <= cur) {
            shrink_used(h, b, need);
            b->request = clamp_request(size);
            return ptr;
        }
        favo_block *n = blk_next(b);
        if (!blk_used(n) && cur + blk_size(n) >= need) {
            favo_node *nf = (favo_node *)n;
            size_t total = cur + blk_size(n);
            favo_tree_remove(h, nf);
            if (total - need >= FAVO_MIN_BLOCK) {
                favo_node *t = (favo_node *)((unsigned char *)b + need);
                favo_list_replace(h, nf, t);
                blk_set(&t->head, total - need, 0, FAVO_MAGIC_FREE);
                t->head.request = 0;
                favo_tree_insert(h, t);
                blk_set(b, need, FAVO_FLAG_USED, FAVO_MAGIC_USED);
                h->splits++;
            } else {
                favo_list_remove(h, nf);
                blk_set(b, total, FAVO_FLAG_USED, FAVO_MAGIC_USED);
            }
            b->request = clamp_request(size);
            h->coalesces++;
            return ptr;
        }
    } else if (need <= cur) {
        b->request = clamp_request(size);
        return ptr;
    }
    void *np = alloc_unlocked(h, size);
    if (!np)
        return NULL;
    size_t keep = cur - FAVO_OVERHEAD;
    memcpy(np, ptr, keep < size ? keep : size);
    free_unlocked(h, ptr);
    return np;
}

static void *memalign_unlocked(favo_heap *h, size_t align, size_t size)
{
    if (align <= FAVO_ALIGNMENT)
        return alloc_unlocked(h, size);
    if (align & (align - 1)) {
        fail(h, FAVO_EINVAL);
        return NULL;
    }
    size_t need;
    if (!block_size_for(size, &need) || align > SIZE_MAX / 4 || need > SIZE_MAX / 4) {
        h->failed_allocs++;
        fail(h, FAVO_ENOMEM);
        return NULL;
    }
    if (h->cfg.mmap_threshold && need + align + FAVO_MIN_BLOCK >= h->cfg.mmap_threshold) {
        void *p = alloc_large(h, need, align, size);
        if (!p) {
            h->failed_allocs++;
            fail(h, FAVO_ENOMEM);
            return NULL;
        }
        h->total_allocs++;
        return p;
    }
    void *p = alloc_unlocked(h, need + align + FAVO_MIN_BLOCK);
    if (!p)
        return NULL;
    favo_block *b = blk_from_payload(p);
    if (((uintptr_t)p & (align - 1)) == 0) {
        shrink_used(h, b, need);
        b->request = clamp_request(size);
        return p;
    }
    uintptr_t aligned = favo_align_up((uintptr_t)p + FAVO_MIN_BLOCK, align);
    favo_block *nb = (favo_block *)(aligned - FAVO_HEADER_SIZE);
    size_t front = (size_t)((unsigned char *)nb - (unsigned char *)b);
    size_t total = blk_size(b);
    blk_set(nb, total - front, FAVO_FLAG_USED, FAVO_MAGIC_USED);
    blk_set(b, front, FAVO_FLAG_USED, FAVO_MAGIC_USED);
    coalesce_insert(h, b);
    h->splits++;
    shrink_used(h, nb, need);
    nb->request = clamp_request(size);
    return blk_payload(nb);
}

favo_heap *favo_heap_create(const favo_config *cfg)
{
    size_t len = favo_align_up(sizeof(favo_heap), favo_os_page());
    favo_heap *h = favo_os_map(len);
    if (!h)
        return NULL;
    favo_config c;
    if (cfg)
        c = *cfg;
    else
        favo_config_default(&c);
    if (c.source == FAVO_SOURCE_AUTO)
        c.source = FAVO_SOURCE_MMAP;
    favo_heap_init_inplace(h, &c);
    h->owned_struct = 1;
    return h;
}

static void release_all(favo_heap *h)
{
    while (h->arenas) {
        favo_arena *top = NULL;
        for (favo_arena *a = h->arenas; a; a = a->next)
            if (a->source == FAVO_SOURCE_SBRK)
                top = a;
        favo_arena *victim = top ? top : h->arenas;
        arena_release(h, victim);
    }
    h->list = NULL;
    h->rover = NULL;
    h->root = NULL;
    h->arena_bytes = 0;
}

void favo_heap_destroy(favo_heap *h)
{
    if (!h)
        return;
    lock(h);
    release_all(h);
    unlock(h);
    if (h->owned_struct) {
        favo_lock_destroy(&h->lock);
        favo_os_unmap(h, favo_align_up(sizeof(favo_heap), favo_os_page()));
    }
}

void favo_heap_reset(favo_heap *h)
{
    lock(h);
    release_all(h);
    h->peak_arena_bytes = 0;
    h->total_allocs = h->total_frees = h->total_reallocs = h->failed_allocs = 0;
    h->splits = h->coalesces = h->os_requests = h->os_releases = h->search_steps = 0;
    h->last_error = FAVO_OK;
    unlock(h);
}

void *favo_heap_alloc(favo_heap *h, size_t size)
{
    lock(h);
    void *p = alloc_unlocked(h, size);
    unlock(h);
    return p;
}

void *favo_heap_calloc(favo_heap *h, size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) {
        lock(h);
        h->failed_allocs++;
        fail(h, FAVO_ENOMEM);
        unlock(h);
        return NULL;
    }
    size_t total = count * size;
    lock(h);
    void *p = alloc_unlocked(h, total);
    unlock(h);
    if (p)
        memset(p, 0, total);
    return p;
}

void *favo_heap_realloc(favo_heap *h, void *ptr, size_t size)
{
    lock(h);
    void *p = realloc_unlocked(h, ptr, size);
    unlock(h);
    return p;
}

void *favo_heap_memalign(favo_heap *h, size_t alignment, size_t size)
{
    lock(h);
    void *p = memalign_unlocked(h, alignment, size);
    unlock(h);
    return p;
}

int favo_heap_free(favo_heap *h, void *ptr)
{
    lock(h);
    int rc = free_unlocked(h, ptr);
    unlock(h);
    return rc;
}

size_t favo_heap_usable_size(favo_heap *h, const void *ptr)
{
    if (!ptr)
        return 0;
    lock(h);
    int saved = h->cfg.abort_on_error;
    h->cfg.abort_on_error = 0;
    favo_block *b = validate(h, ptr, NULL);
    h->cfg.abort_on_error = saved;
    size_t n = b ? blk_size(b) - FAVO_OVERHEAD : 0;
    unlock(h);
    return n;
}

int favo_heap_owns(favo_heap *h, const void *ptr)
{
    lock(h);
    int r = favo_arena_of(h, ptr) != NULL;
    unlock(h);
    return r;
}

int favo_heap_set_policy(favo_heap *h, int policy)
{
    if (policy < FAVO_FIRST_FIT || policy > FAVO_BEST_FIT)
        return FAVO_EINVAL;
    lock(h);
    h->cfg.policy = policy;
    h->rover = h->list;
    unlock(h);
    return FAVO_OK;
}

int favo_heap_get_policy(favo_heap *h)
{
    return h->cfg.policy;
}

size_t favo_heap_trim(favo_heap *h, size_t pad)
{
    size_t released = 0;
    lock(h);
    favo_arena *a = h->arenas;
    while (a) {
        favo_arena *next = a->next;
        released += trim_arena(h, a, pad);
        a = next;
    }
    unlock(h);
    return released;
}

int favo_heap_last_error(favo_heap *h)
{
    return h->last_error;
}
