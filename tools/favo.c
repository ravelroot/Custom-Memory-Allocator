#include "cli.h"

#include <stdlib.h>
#include <string.h>

typedef struct options {
    int policy;
    int order;
    int check;
    int map;
    int blocks;
    int quiet;
    size_t ops;
    uint64_t seed;
    size_t min_size;
    size_t max_size;
    size_t threshold;
    const char *out;
    const char *file;
} options;

static int parse_policy(const char *v)
{
    if (!strcmp(v, "next") || !strcmp(v, "next-fit"))
        return FAVO_NEXT_FIT;
    if (!strcmp(v, "best") || !strcmp(v, "best-fit"))
        return FAVO_BEST_FIT;
    if (!strcmp(v, "first") || !strcmp(v, "first-fit"))
        return FAVO_FIRST_FIT;
    return -1;
}

static void usage(FILE *out)
{
    fprintf(out,
        "favo %s - alocador de memoria personalizado\n\n"
        "uso:\n"
        "  favo run <trace> [--policy first|next|best] [--order address|lifo] [--check] [--map] [--blocks]\n"
        "  favo compare <trace> [--check]\n"
        "  favo bench [--ops N] [--seed S] [--min B] [--max B]\n"
        "  favo gen [--ops N] [--seed S] [--min B] [--max B] [--out arquivo]\n"
        "  favo demo [--policy first|next|best]\n"
        "  favo version\n\n"
        "formato do trace (um comando por linha):\n"
        "  a <id> <bytes>            aloca\n"
        "  c <id> <qtd> <bytes>      aloca zerado (calloc)\n"
        "  m <id> <alinhamento> <b>  aloca alinhado (memalign)\n"
        "  r <id> <bytes>            realoca\n"
        "  f <id>                    libera\n",
        favo_version());
}

static int parse_options(int argc, char **argv, int start, options *o)
{
    for (int i = start; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--check")) {
            o->check = 1;
        } else if (!strcmp(a, "--map")) {
            o->map = 1;
        } else if (!strcmp(a, "--blocks")) {
            o->map = 1;
            o->blocks = 1;
        } else if (!strcmp(a, "--quiet")) {
            o->quiet = 1;
        } else if (!strcmp(a, "--policy") && v) {
            o->policy = parse_policy(v);
            if (o->policy < 0) {
                fprintf(stderr, "politica desconhecida: %s\n", v);
                return -1;
            }
            i++;
        } else if (!strcmp(a, "--order") && v) {
            o->order = !strcmp(v, "lifo") ? FAVO_ORDER_LIFO : FAVO_ORDER_ADDRESS;
            i++;
        } else if (!strcmp(a, "--ops") && v) {
            o->ops = (size_t)strtoull(v, NULL, 10);
            i++;
        } else if (!strcmp(a, "--seed") && v) {
            o->seed = strtoull(v, NULL, 10);
            i++;
        } else if (!strcmp(a, "--min") && v) {
            o->min_size = (size_t)strtoull(v, NULL, 10);
            i++;
        } else if (!strcmp(a, "--max") && v) {
            o->max_size = (size_t)strtoull(v, NULL, 10);
            i++;
        } else if (!strcmp(a, "--threshold") && v) {
            o->threshold = (size_t)strtoull(v, NULL, 10);
            i++;
        } else if (!strcmp(a, "--out") && v) {
            o->out = v;
            i++;
        } else if (a[0] != '-' && !o->file) {
            o->file = a;
        } else {
            fprintf(stderr, "opcao invalida: %s\n", a);
            return -1;
        }
    }
    return 0;
}

static favo_heap *make_heap(const options *o, int policy)
{
    favo_config cfg;
    favo_config_default(&cfg);
    cfg.policy = policy;
    cfg.order = o->order;
    cfg.source = FAVO_SOURCE_MMAP;
    cfg.grow_size = 64 * 1024;
    if (o->threshold)
        cfg.mmap_threshold = o->threshold;
    return favo_heap_create(&cfg);
}

static int cmd_run(const options *o)
{
    if (!o->file) {
        fprintf(stderr, "informe o arquivo de trace\n");
        return 2;
    }
    trace t;
    char err[256];
    if (trace_load(o->file, &t, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return 2;
    }
    favo_heap *h = make_heap(o, o->policy);
    trace_result r;
    trace_run(h, &t, o->check, &r);
    if (!o->quiet) {
        printf("trace             %s (%zu comandos)\n", o->file, t.count);
        printf("executados        %zu (%zu falhas, %zu ignorados) em %.3f s\n", r.executed, r.failed, r.invalid, r.seconds);
        stats_print(stdout, h);
        if (o->map) {
            printf("\n");
            heapmap_print(stdout, h, 72, o->blocks);
        }
    }
    if (r.check_status) {
        printf("verificacao       FALHOU na linha %zu: %s\n", r.check_line, r.check_message);
    } else {
        printf("verificacao       ok\n");
    }
    int rc = r.check_status ? 1 : 0;
    favo_heap_destroy(h);
    trace_dispose(&t);
    return rc;
}

static int cmd_compare(const options *o)
{
    if (!o->file) {
        fprintf(stderr, "informe o arquivo de trace\n");
        return 2;
    }
    trace t;
    char err[256];
    if (trace_load(o->file, &t, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return 2;
    }
    int rc = 0;
    printf("%-10s %12s %12s %12s %10s %10s %8s\n", "politica", "pico (KiB)", "passos", "fusoes", "fragm.", "aprov.", "status");
    for (int p = FAVO_FIRST_FIT; p <= FAVO_BEST_FIT; p++) {
        favo_heap *h = make_heap(o, p);
        trace_result r;
        trace_run(h, &t, o->check, &r);
        favo_stats s;
        favo_heap_stats(h, &s);
        printf("%-10s %12.1f %12llu %12llu %9.1f%% %9.1f%% %8s\n", favo_policy_name(p), (double)s.peak_arena_bytes / 1024.0,
            (unsigned long long)s.search_steps, (unsigned long long)s.coalesces, s.fragmentation * 100.0,
            s.utilization * 100.0, r.check_status ? "erro" : "ok");
        if (r.check_status)
            rc = 1;
        favo_heap_destroy(h);
    }
    trace_dispose(&t);
    return rc;
}

static int cmd_bench(const options *o)
{
    size_t ops = o->ops ? o->ops : 200000;
    size_t min = o->min_size ? o->min_size : 16;
    size_t max = o->max_size ? o->max_size : 4096;
    printf("carga aleatoria: %zu operacoes, blocos de %zu a %zu bytes, semente %llu\n\n", ops, min, max, (unsigned long long)o->seed);
    printf("%-14s %14s %12s %14s %10s %10s\n", "alocador", "ops/s", "tempo (s)", "pico (KiB)", "fragm.", "aprov.");
    for (int p = FAVO_FIRST_FIT; p <= FAVO_BEST_FIT; p++) {
        favo_bench_result r;
        if (favo_bench_run(p, ops, o->seed, min, max, &r))
            continue;
        printf("favo %-9s %14.0f %12.3f %14.1f %9.1f%% %9.1f%%\n", favo_policy_name(p), r.ops_per_second, r.seconds,
            (double)r.peak_arena_bytes / 1024.0, r.fragmentation * 100.0, r.utilization * 100.0);
    }
    favo_bench_result sys;
    if (!favo_bench_system(ops, o->seed, min, max, &sys))
        printf("%-14s %14.0f %12.3f %14s %10s %10s\n", "malloc sistema", sys.ops_per_second, sys.seconds, "-", "-", "-");
    return 0;
}

static int cmd_gen(const options *o)
{
    FILE *out = stdout;
    if (o->out) {
        out = fopen(o->out, "w");
        if (!out) {
            fprintf(stderr, "nao foi possivel criar %s\n", o->out);
            return 2;
        }
    }
    trace_generate(out, o->ops ? o->ops : 1000, o->seed, o->min_size ? o->min_size : 16, o->max_size ? o->max_size : 2048);
    if (out != stdout)
        fclose(out);
    return 0;
}

static int cmd_demo(const options *o)
{
    favo_config cfg;
    favo_config_default(&cfg);
    cfg.policy = o->policy;
    cfg.source = FAVO_SOURCE_MMAP;
    cfg.grow_size = 16 * 1024;
    favo_heap *h = favo_heap_create(&cfg);
    void *p[8];
    size_t sizes[8] = { 1200, 300, 2000, 120, 900, 64, 3000, 500 };
    for (int i = 0; i < 8; i++)
        p[i] = favo_heap_alloc(h, sizes[i]);
    printf("1) oito blocos alocados em sequencia\n");
    heapmap_print(stdout, h, 64, 0);
    favo_heap_free(h, p[1]);
    favo_heap_free(h, p[3]);
    favo_heap_free(h, p[5]);
    printf("\n2) blocos 1, 3 e 5 liberados: buracos no meio do heap\n");
    heapmap_print(stdout, h, 64, 0);
    void *q = favo_heap_alloc(h, 100);
    printf("\n3) pedido de 100 bytes com %s: escolhido o endereco 0x%llx\n", favo_policy_name(o->policy), (unsigned long long)(uintptr_t)q);
    heapmap_print(stdout, h, 64, 0);
    favo_heap_free(h, p[2]);
    printf("\n4) bloco 2 liberado: fundido com os vizinhos livres (coalescing)\n");
    heapmap_print(stdout, h, 64, 1);
    printf("\n");
    stats_print(stdout, h);
    char msg[256];
    int rc = favo_heap_check(h, msg, sizeof(msg));
    printf("verificacao       %s\n", rc ? msg : "ok");
    favo_heap_destroy(h);
    return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stdout);
        return 0;
    }
    options o;
    memset(&o, 0, sizeof(o));
    o.policy = FAVO_FIRST_FIT;
    o.order = FAVO_ORDER_ADDRESS;
    const char *cmd = argv[1];
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
        printf("favo %s\n", favo_version());
        return 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(stdout);
        return 0;
    }
    if (parse_options(argc, argv, 2, &o))
        return 2;
    if (!strcmp(cmd, "run"))
        return cmd_run(&o);
    if (!strcmp(cmd, "compare"))
        return cmd_compare(&o);
    if (!strcmp(cmd, "bench"))
        return cmd_bench(&o);
    if (!strcmp(cmd, "gen"))
        return cmd_gen(&o);
    if (!strcmp(cmd, "demo"))
        return cmd_demo(&o);
    usage(stderr);
    return 2;
}
