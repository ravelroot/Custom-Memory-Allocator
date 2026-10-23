#include "internal.h"

#define RB_RED ((uintptr_t)1)

favo_node *favo_tree_parent(const favo_node *n)
{
    return (favo_node *)(n->parent_color & ~RB_RED);
}

int favo_tree_is_red(const favo_node *n)
{
    return n && (n->parent_color & RB_RED);
}

static void set_parent(favo_node *n, favo_node *p)
{
    n->parent_color = (uintptr_t)p | (n->parent_color & RB_RED);
}

static void set_red(favo_node *n)
{
    n->parent_color |= RB_RED;
}

static void set_black(favo_node *n)
{
    n->parent_color &= ~RB_RED;
}

static int less(const favo_node *a, const favo_node *b)
{
    size_t sa = blk_size(&a->head);
    size_t sb = blk_size(&b->head);
    if (sa != sb)
        return sa < sb;
    return (uintptr_t)a < (uintptr_t)b;
}

static void rotate_left(favo_heap *h, favo_node *x)
{
    favo_node *y = x->right;
    favo_node *p = favo_tree_parent(x);
    x->right = y->left;
    if (y->left)
        set_parent(y->left, x);
    set_parent(y, p);
    if (!p)
        h->root = y;
    else if (x == p->left)
        p->left = y;
    else
        p->right = y;
    y->left = x;
    set_parent(x, y);
}

static void rotate_right(favo_heap *h, favo_node *x)
{
    favo_node *y = x->left;
    favo_node *p = favo_tree_parent(x);
    x->left = y->right;
    if (y->right)
        set_parent(y->right, x);
    set_parent(y, p);
    if (!p)
        h->root = y;
    else if (x == p->right)
        p->right = y;
    else
        p->left = y;
    y->right = x;
    set_parent(x, y);
}

void favo_tree_insert(favo_heap *h, favo_node *z)
{
    favo_node *y = NULL;
    favo_node *x = h->root;
    while (x) {
        y = x;
        x = less(z, x) ? x->left : x->right;
    }
    z->left = NULL;
    z->right = NULL;
    z->parent_color = (uintptr_t)y | RB_RED;
    if (!y)
        h->root = z;
    else if (less(z, y))
        y->left = z;
    else
        y->right = z;

    favo_node *p;
    while ((p = favo_tree_parent(z)) && favo_tree_is_red(p)) {
        favo_node *g = favo_tree_parent(p);
        if (p == g->left) {
            favo_node *u = g->right;
            if (favo_tree_is_red(u)) {
                set_black(p);
                set_black(u);
                set_red(g);
                z = g;
            } else {
                if (z == p->right) {
                    z = p;
                    rotate_left(h, z);
                    p = favo_tree_parent(z);
                }
                set_black(p);
                set_red(g);
                rotate_right(h, g);
            }
        } else {
            favo_node *u = g->left;
            if (favo_tree_is_red(u)) {
                set_black(p);
                set_black(u);
                set_red(g);
                z = g;
            } else {
                if (z == p->left) {
                    z = p;
                    rotate_right(h, z);
                    p = favo_tree_parent(z);
                }
                set_black(p);
                set_red(g);
                rotate_left(h, g);
            }
        }
    }
    set_black(h->root);
}

static void transplant(favo_heap *h, favo_node *u, favo_node *v)
{
    favo_node *p = favo_tree_parent(u);
    if (!p)
        h->root = v;
    else if (u == p->left)
        p->left = v;
    else
        p->right = v;
    if (v)
        set_parent(v, p);
}

static favo_node *minimum(favo_node *n)
{
    while (n->left)
        n = n->left;
    return n;
}

static void remove_fixup(favo_heap *h, favo_node *x, favo_node *xp)
{
    while (x != h->root && !favo_tree_is_red(x)) {
        if (x == xp->left) {
            favo_node *w = xp->right;
            if (favo_tree_is_red(w)) {
                set_black(w);
                set_red(xp);
                rotate_left(h, xp);
                w = xp->right;
            }
            if (!favo_tree_is_red(w->left) && !favo_tree_is_red(w->right)) {
                set_red(w);
                x = xp;
                xp = favo_tree_parent(x);
            } else {
                if (!favo_tree_is_red(w->right)) {
                    set_black(w->left);
                    set_red(w);
                    rotate_right(h, w);
                    w = xp->right;
                }
                if (favo_tree_is_red(xp))
                    set_red(w);
                else
                    set_black(w);
                set_black(xp);
                set_black(w->right);
                rotate_left(h, xp);
                x = h->root;
                xp = NULL;
            }
        } else {
            favo_node *w = xp->left;
            if (favo_tree_is_red(w)) {
                set_black(w);
                set_red(xp);
                rotate_right(h, xp);
                w = xp->left;
            }
            if (!favo_tree_is_red(w->left) && !favo_tree_is_red(w->right)) {
                set_red(w);
                x = xp;
                xp = favo_tree_parent(x);
            } else {
                if (!favo_tree_is_red(w->left)) {
                    set_black(w->right);
                    set_red(w);
                    rotate_left(h, w);
                    w = xp->left;
                }
                if (favo_tree_is_red(xp))
                    set_red(w);
                else
                    set_black(w);
                set_black(xp);
                set_black(w->left);
                rotate_right(h, xp);
                x = h->root;
                xp = NULL;
            }
        }
    }
    if (x)
        set_black(x);
}

void favo_tree_remove(favo_heap *h, favo_node *z)
{
    favo_node *y = z;
    favo_node *x;
    favo_node *xp;
    int y_red = favo_tree_is_red(y);

    if (!z->left) {
        x = z->right;
        xp = favo_tree_parent(z);
        transplant(h, z, z->right);
    } else if (!z->right) {
        x = z->left;
        xp = favo_tree_parent(z);
        transplant(h, z, z->left);
    } else {
        y = minimum(z->right);
        y_red = favo_tree_is_red(y);
        x = y->right;
        if (favo_tree_parent(y) == z) {
            xp = y;
        } else {
            xp = favo_tree_parent(y);
            transplant(h, y, y->right);
            y->right = z->right;
            set_parent(y->right, y);
        }
        transplant(h, z, y);
        y->left = z->left;
        set_parent(y->left, y);
        if (favo_tree_is_red(z))
            set_red(y);
        else
            set_black(y);
    }
    if (!y_red)
        remove_fixup(h, x, xp);
    z->left = NULL;
    z->right = NULL;
    z->parent_color = 0;
}

favo_node *favo_tree_lower_bound(favo_heap *h, size_t size, uint64_t *steps)
{
    favo_node *n = h->root;
    favo_node *best = NULL;
    while (n) {
        (*steps)++;
        if (blk_size(&n->head) >= size) {
            best = n;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return best;
}
