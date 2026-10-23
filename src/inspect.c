#include "internal.h"

#include <stdio.h>

static void hold(favo_heap *h)
{
    if (h->cfg.thread_safe)
        favo_lock_acquire(&h->lock);
}

static void drop(favo_heap *h)
{
    if (h->cfg.thread_safe)
        favo_lock_release(&h->lock);
}

void favo_heap_stats(favo_heap *h, favo_stats *s)
{
    memset(s, 0, sizeof(*s));
    hold(h);
    for (favo_arena *a = h->arenas; a; a = a->next) {
        s->arena_count++;
        for (favo_block *b = (favo_block *)a->first; (unsigned char *)b < a->end; b = blk_next(b)) {
            size_t size = blk_size(b);
            if (blk_used(b)) {
                s->used_blocks++;
                s->used_bytes += size;
                s->requested_bytes += b->request;
                if (blk_large(b)) {
                    s->large_blocks++;
                    s->large_bytes += size;
                }
            } else {
                s->free_blocks++;
                s->free_bytes += size;
                if (size > s->largest_free)
                    s->largest_free = size;
            }
        }
    }
    s->arena_bytes = h->arena_bytes;
    s->peak_arena_bytes = h->peak_arena_bytes;
    s->total_allocs = h->total_allocs;
    s->total_frees = h->total_frees;
    s->total_reallocs = h->total_reallocs;
    s->failed_allocs = h->failed_allocs;
    s->splits = h->splits;
    s->coalesces = h->coalesces;
    s->os_requests = h->os_requests;
    s->os_releases = h->os_releases;
    s->search_steps = h->search_steps;
    drop(h);
    s->fragmentation = s->free_bytes ? 1.0 - (double)s->largest_free / (double)s->free_bytes : 0.0;
    s->utilization = s->arena_bytes ? (double)s->requested_bytes / (double)s->arena_bytes : 0.0;
}

size_t favo_heap_snapshot(favo_heap *h, favo_block_info *out, size_t max)
{
    size_t n = 0;
    size_t index = 0;
    hold(h);
    for (favo_arena *a = h->arenas; a; a = a->next, index++) {
        for (favo_block *b = (favo_block *)a->first; (unsigned char *)b < a->end; b = blk_next(b)) {
            if (out && n < max) {
                favo_block_info *i = &out[n];
                i->arena = index;
                i->offset = (size_t)((unsigned char *)b - a->base);
                i->size = blk_size(b);
                i->payload = blk_size(b) - FAVO_OVERHEAD;
                i->request = blk_used(b) ? b->request : 0;
                i->used = blk_used(b);
                i->large = blk_large(b);
                i->address = (uintptr_t)blk_payload(b);
            }
            n++;
        }
    }
    drop(h);
    return n;
}

size_t favo_heap_arenas(favo_heap *h, favo_arena_info *out, size_t max)
{
    size_t n = 0;
    hold(h);
    for (favo_arena *a = h->arenas; a; a = a->next) {
        if (out && n < max) {
            out[n].index = n;
            out[n].base = (uintptr_t)a->base;
            out[n].length = a->length;
            out[n].first_offset = (size_t)(a->first - a->base);
            out[n].end_offset = (size_t)(a->end - a->base);
            out[n].source = a->source;
            out[n].large = a->large;
        }
        n++;
    }
    drop(h);
    return n;
}

size_t favo_heap_free_list(favo_heap *h, uintptr_t *out, size_t max)
{
    size_t n = 0;
    hold(h);
    favo_node *f = h->list;
    if (f) {
        do {
            if (out && n < max)
                out[n] = (uintptr_t)f;
            n++;
            f = f->next;
        } while (f != h->list);
    }
    drop(h);
    return n;
}

size_t favo_heap_tree(favo_heap *h, favo_tree_node *out, size_t max)
{
    favo_node *stack[256];
    int depth[256];
    int top = 0;
    size_t n = 0;
    hold(h);
    if (h->root) {
        stack[top] = h->root;
        depth[top] = 0;
        top++;
    }
    while (top > 0) {
        top--;
        favo_node *f = stack[top];
        int d = depth[top];
        if (out && n < max) {
            out[n].address = (uintptr_t)f;
            out[n].left = (uintptr_t)f->left;
            out[n].right = (uintptr_t)f->right;
            out[n].size = blk_size(&f->head);
            out[n].red = favo_tree_is_red(f);
            out[n].depth = d;
        }
        n++;
        if (f->right && top < 256) {
            stack[top] = f->right;
            depth[top] = d + 1;
            top++;
        }
        if (f->left && top < 256) {
            stack[top] = f->left;
            depth[top] = d + 1;
            top++;
        }
    }
    drop(h);
    return n;
}

typedef struct check_ctx {
    favo_heap *h;
    char *msg;
    size_t len;
    size_t nodes;
} check_ctx;

static int report(check_ctx *c, const char *text, const void *where)
{
    if (c->msg && c->len)
        snprintf(c->msg, c->len, "%s (%p)", text, where);
    return FAVO_ECORRUPT;
}

static int tree_black_height(check_ctx *c, favo_node *n, favo_node *parent, int *out)
{
    if (!n) {
        *out = 1;
        return FAVO_OK;
    }
    c->nodes++;
    if (favo_tree_parent(n) != parent)
        return report(c, "arvore: ponteiro de pai inconsistente", n);
    if (blk_used(&n->head) || n->head.magic != FAVO_MAGIC_FREE)
        return report(c, "arvore: no nao e um bloco livre", n);
    if (favo_tree_is_red(n) && (favo_tree_is_red(n->left) || favo_tree_is_red(n->right)))
        return report(c, "arvore: no vermelho com filho vermelho", n);
    size_t size = blk_size(&n->head);
    if (n->left) {
        size_t ls = blk_size(&n->left->head);
        if (ls > size || (ls == size && (uintptr_t)n->left > (uintptr_t)n))
            return report(c, "arvore: ordem violada a esquerda", n);
    }
    if (n->right) {
        size_t rs = blk_size(&n->right->head);
        if (rs < size || (rs == size && (uintptr_t)n->right < (uintptr_t)n))
            return report(c, "arvore: ordem violada a direita", n);
    }
    int lh = 0;
    int rh = 0;
    int rc = tree_black_height(c, n->left, n, &lh);
    if (rc)
        return rc;
    rc = tree_black_height(c, n->right, n, &rh);
    if (rc)
        return rc;
    if (lh != rh)
        return report(c, "arvore: altura negra diferente entre ramos", n);
    *out = lh + (favo_tree_is_red(n) ? 0 : 1);
    return FAVO_OK;
}

int favo_heap_check(favo_heap *h, char *message, size_t length)
{
    check_ctx c = { h, message, length, 0 };
    int rc = FAVO_OK;
    size_t free_blocks = 0;
    size_t bytes = 0;
    if (message && length)
        message[0] = '\0';
    hold(h);
    for (favo_arena *a = h->arenas; a && !rc; a = a->next) {
        bytes += a->length;
        if (((uintptr_t)a->first + FAVO_HEADER_SIZE) & (FAVO_ALIGNMENT - 1)) {
            rc = report(&c, "arena: primeiro bloco desalinhado", a->first);
            break;
        }
        if (!(*((size_t *)a->first - 1) & FAVO_FLAG_USED)) {
            rc = report(&c, "arena: prologo corrompido", a->first);
            break;
        }
        favo_block *e = (favo_block *)a->end;
        if (blk_size(e) != 0 || !blk_used(e) || e->magic != FAVO_MAGIC_EDGE) {
            rc = report(&c, "arena: epilogo corrompido", a->end);
            break;
        }
        int prev_free = 0;
        favo_block *b = (favo_block *)a->first;
        while ((unsigned char *)b < a->end) {
            size_t size = blk_size(b);
            if (size < FAVO_MIN_BLOCK || (size & (FAVO_ALIGNMENT - 1)) || size > (size_t)(a->end - (unsigned char *)b)) {
                rc = report(&c, "bloco: tamanho invalido", b);
                break;
            }
            if (((uintptr_t)blk_payload(b)) & (FAVO_ALIGNMENT - 1)) {
                rc = report(&c, "bloco: payload desalinhado", b);
                break;
            }
            if (*blk_footer(b) != b->info) {
                rc = report(&c, "bloco: rodape diferente do cabecalho", b);
                break;
            }
            if (blk_used(b)) {
                if (b->magic != FAVO_MAGIC_USED) {
                    rc = report(&c, "bloco: assinatura de bloco ocupado invalida", b);
                    break;
                }
                if (blk_large(b) != a->large) {
                    rc = report(&c, "bloco: marca de bloco grande inconsistente", b);
                    break;
                }
                prev_free = 0;
            } else {
                if (b->magic != FAVO_MAGIC_FREE) {
                    rc = report(&c, "bloco: assinatura de bloco livre invalida", b);
                    break;
                }
                if (prev_free) {
                    rc = report(&c, "fusao: dois blocos livres vizinhos", b);
                    break;
                }
                prev_free = 1;
                free_blocks++;
            }
            b = blk_next(b);
        }
        if (!rc && (unsigned char *)b != a->end)
            rc = report(&c, "arena: blocos nao terminam no epilogo", b);
    }
    if (!rc && bytes != h->arena_bytes)
        rc = report(&c, "contador: bytes de arena divergentes", h);
    if (!rc && h->list) {
        size_t count = 0;
        favo_node *f = h->list;
        do {
            if (f->next->prev != f) {
                rc = report(&c, "lista: encadeamento circular quebrado", f);
                break;
            }
            if (blk_used(&f->head) || f->head.magic != FAVO_MAGIC_FREE || !favo_arena_of(h, f)) {
                rc = report(&c, "lista: contem bloco que nao esta livre", f);
                break;
            }
            if (h->cfg.order == FAVO_ORDER_ADDRESS && f->next != h->list && (uintptr_t)f->next < (uintptr_t)f) {
                rc = report(&c, "lista: ordem por endereco violada", f);
                break;
            }
            count++;
            if (count > free_blocks) {
                rc = report(&c, "lista: mais nos que blocos livres", f);
                break;
            }
            f = f->next;
        } while (f != h->list);
        if (!rc && count != free_blocks)
            rc = report(&c, "lista: quantidade diferente dos blocos livres", h->list);
    } else if (!rc && free_blocks) {
        rc = report(&c, "lista: vazia com blocos livres no heap", h);
    }
    if (!rc && h->root) {
        int bh = 0;
        if (favo_tree_is_red(h->root))
            rc = report(&c, "arvore: raiz vermelha", h->root);
        else if (favo_tree_parent(h->root))
            rc = report(&c, "arvore: raiz com pai", h->root);
        else
            rc = tree_black_height(&c, h->root, NULL, &bh);
        if (!rc && c.nodes != free_blocks)
            rc = report(&c, "arvore: quantidade diferente dos blocos livres", h->root);
    } else if (!rc && free_blocks) {
        rc = report(&c, "arvore: vazia com blocos livres no heap", h);
    }
    drop(h);
    return rc;
}
