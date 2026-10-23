#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "cli.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int push(trace *t, trace_op op)
{
    if (t->count == t->capacity) {
        size_t cap = t->capacity ? t->capacity * 2 : 256;
        trace_op *ops = (trace_op *)realloc(t->ops, cap * sizeof(trace_op));
        if (!ops)
            return 0;
        t->ops = ops;
        t->capacity = cap;
    }
    t->ops[t->count++] = op;
    if (op.id > t->max_id)
        t->max_id = op.id;
    return 1;
}

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
    return p;
}

static int read_number(const char **cursor, size_t *out)
{
    const char *p = skip_space(*cursor);
    if (!isdigit((unsigned char)*p))
        return 0;
    char *end = NULL;
    unsigned long long n = strtoull(p, &end, 10);
    if (*end == 'k' || *end == 'K') {
        n <<= 10;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        n <<= 20;
        end++;
    }
    *out = (size_t)n;
    *cursor = end;
    return 1;
}

int trace_parse(const char *text, trace *out, char *error, size_t length)
{
    memset(out, 0, sizeof(*out));
    size_t line = 0;
    const char *p = text;
    while (*p) {
        line++;
        const char *eol = strchr(p, '\n');
        const char *next = eol ? eol + 1 : p + strlen(p);
        const char *c = skip_space(p);
        if (*c && *c != '\n' && *c != '#' && *c != ';') {
            trace_op op;
            memset(&op, 0, sizeof(op));
            op.kind = (char)tolower((unsigned char)*c);
            op.line = line;
            c++;
            int ok = read_number(&c, &op.id);
            switch (op.kind) {
            case TRACE_ALLOC:
            case TRACE_REALLOC:
                ok = ok && read_number(&c, &op.size);
                break;
            case TRACE_CALLOC:
            case TRACE_MEMALIGN:
                ok = ok && read_number(&c, &op.extra) && read_number(&c, &op.size);
                break;
            case TRACE_FREE:
                break;
            default:
                ok = 0;
            }
            if (ok && op.id > 1000000)
                ok = 0;
            if (!ok) {
                if (error && length)
                    snprintf(error, length, "linha %zu: comando invalido", line);
                trace_dispose(out);
                return -1;
            }
            if (!push(out, op)) {
                if (error && length)
                    snprintf(error, length, "memoria insuficiente ao ler o trace");
                trace_dispose(out);
                return -1;
            }
        }
        p = next;
    }
    return 0;
}

int trace_load(const char *path, trace *out, char *error, size_t length)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (error && length)
            snprintf(error, length, "nao foi possivel abrir %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        fclose(f);
        return -1;
    }
    size_t got = fread(text, 1, (size_t)size, f);
    text[got] = '\0';
    fclose(f);
    int rc = trace_parse(text, out, error, length);
    free(text);
    return rc;
}

void trace_dispose(trace *t)
{
    free(t->ops);
    memset(t, 0, sizeof(*t));
}

static double clock_now(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

int trace_run(favo_heap *heap, const trace *t, int check_each, trace_result *out)
{
    memset(out, 0, sizeof(*out));
    void **slots = (void **)calloc(t->max_id + 1, sizeof(void *));
    size_t *sizes = (size_t *)calloc(t->max_id + 1, sizeof(size_t));
    if (!slots || !sizes) {
        free(slots);
        free(sizes);
        return -1;
    }
    double start = clock_now();
    for (size_t i = 0; i < t->count; i++) {
        const trace_op *op = &t->ops[i];
        void *p = NULL;
        switch (op->kind) {
        case TRACE_ALLOC:
        case TRACE_CALLOC:
        case TRACE_MEMALIGN:
            if (slots[op->id]) {
                out->invalid++;
                continue;
            }
            if (op->kind == TRACE_ALLOC)
                p = favo_heap_alloc(heap, op->size);
            else if (op->kind == TRACE_CALLOC)
                p = favo_heap_calloc(heap, op->extra, op->size);
            else
                p = favo_heap_memalign(heap, op->extra, op->size);
            if (!p) {
                out->failed++;
                break;
            }
            slots[op->id] = p;
            sizes[op->id] = op->kind == TRACE_CALLOC ? op->extra * op->size : op->size;
            if (sizes[op->id])
                memset(p, (int)(op->id & 0xFF), sizes[op->id]);
            break;
        case TRACE_REALLOC:
            p = favo_heap_realloc(heap, slots[op->id], op->size);
            if (!p && op->size) {
                out->failed++;
                break;
            }
            slots[op->id] = p;
            sizes[op->id] = op->size;
            break;
        case TRACE_FREE:
            if (!slots[op->id]) {
                out->invalid++;
                continue;
            }
            favo_heap_free(heap, slots[op->id]);
            slots[op->id] = NULL;
            sizes[op->id] = 0;
            break;
        default:
            break;
        }
        out->executed++;
        if (check_each) {
            int rc = favo_heap_check(heap, out->check_message, sizeof(out->check_message));
            if (rc) {
                out->check_status = rc;
                out->check_line = op->line;
                break;
            }
        }
    }
    out->seconds = clock_now() - start;
    if (!out->check_status)
        out->check_status = favo_heap_check(heap, out->check_message, sizeof(out->check_message));
    free(slots);
    free(sizes);
    return 0;
}

static uint64_t rnd(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

int trace_generate(FILE *out, size_t operations, uint64_t seed, size_t min_size, size_t max_size)
{
    size_t slots = operations / 4 + 8;
    unsigned char *live = (unsigned char *)calloc(slots, 1);
    if (!live)
        return -1;
    uint64_t s = seed ? seed : 42;
    if (max_size < min_size)
        max_size = min_size;
    for (size_t i = 0; i < operations; i++) {
        size_t id = (size_t)(rnd(&s) % slots);
        size_t size = min_size + (size_t)(rnd(&s) % (max_size - min_size + 1));
        if (!live[id]) {
            fprintf(out, "a %zu %zu\n", id, size);
            live[id] = 1;
        } else if (rnd(&s) % 4) {
            fprintf(out, "f %zu\n", id);
            live[id] = 0;
        } else {
            fprintf(out, "r %zu %zu\n", id, size);
        }
    }
    free(live);
    return 0;
}
