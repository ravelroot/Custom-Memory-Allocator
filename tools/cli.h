#ifndef FAVO_CLI_H
#define FAVO_CLI_H

#include "favo.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum trace_kind {
    TRACE_ALLOC = 'a',
    TRACE_CALLOC = 'c',
    TRACE_REALLOC = 'r',
    TRACE_MEMALIGN = 'm',
    TRACE_FREE = 'f'
} trace_kind;

typedef struct trace_op {
    char kind;
    size_t id;
    size_t size;
    size_t extra;
    size_t line;
} trace_op;

typedef struct trace {
    trace_op *ops;
    size_t count;
    size_t capacity;
    size_t max_id;
} trace;

typedef struct trace_result {
    size_t executed;
    size_t failed;
    size_t invalid;
    int check_status;
    size_t check_line;
    char check_message[256];
    double seconds;
} trace_result;

int trace_load(const char *path, trace *out, char *error, size_t length);
int trace_parse(const char *text, trace *out, char *error, size_t length);
void trace_dispose(trace *t);
int trace_run(favo_heap *heap, const trace *t, int check_each, trace_result *out);
int trace_generate(FILE *out, size_t operations, uint64_t seed, size_t min_size, size_t max_size);

void heapmap_print(FILE *out, favo_heap *heap, int width, int with_blocks);
void stats_print(FILE *out, favo_heap *heap);

#endif
