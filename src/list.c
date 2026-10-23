#include "internal.h"

#define FAVO_SCAN_LIMIT 64

static favo_node *find_successor(favo_heap *h, favo_node *f)
{
    favo_block *b = blk_next(&f->head);
    for (int i = 0; i < FAVO_SCAN_LIMIT; i++) {
        if (blk_size(b) == 0)
            break;
        if (!blk_used(b))
            return (favo_node *)b;
        b = blk_next(b);
    }
    favo_node *n = h->list;
    do {
        if ((uintptr_t)n > (uintptr_t)f)
            return n;
        n = n->next;
    } while (n != h->list);
    return NULL;
}

static void link_before(favo_node *s, favo_node *f)
{
    f->next = s;
    f->prev = s->prev;
    s->prev->next = f;
    s->prev = f;
}

void favo_list_insert(favo_heap *h, favo_node *f)
{
    if (!h->list) {
        f->next = f;
        f->prev = f;
        h->list = f;
        return;
    }
    if (h->cfg.order == FAVO_ORDER_LIFO) {
        link_before(h->list, f);
        h->list = f;
        return;
    }
    favo_node *s = find_successor(h, f);
    if (!s) {
        link_before(h->list, f);
        return;
    }
    link_before(s, f);
    if (s == h->list)
        h->list = f;
}

void favo_list_remove(favo_heap *h, favo_node *f)
{
    if (f->next == f) {
        h->list = NULL;
        h->rover = NULL;
    } else {
        f->prev->next = f->next;
        f->next->prev = f->prev;
        if (h->list == f)
            h->list = f->next;
        if (h->rover == f)
            h->rover = f->next;
    }
    f->next = NULL;
    f->prev = NULL;
}

void favo_list_replace(favo_heap *h, favo_node *old, favo_node *repl)
{
    favo_node *next = old->next;
    favo_node *prev = old->prev;
    if (next == old) {
        repl->next = repl;
        repl->prev = repl;
    } else {
        repl->next = next;
        repl->prev = prev;
        prev->next = repl;
        next->prev = repl;
    }
    if (h->list == old)
        h->list = repl;
    if (h->rover == old)
        h->rover = repl;
}
