#include "cli.h"

#include <stdlib.h>
#include <string.h>

static const char *human(size_t bytes, char *buf, size_t len)
{
    if (bytes >= ((size_t)1 << 20))
        snprintf(buf, len, "%.1f MiB", (double)bytes / (double)(1 << 20));
    else if (bytes >= 1024)
        snprintf(buf, len, "%.1f KiB", (double)bytes / 1024.0);
    else
        snprintf(buf, len, "%zu B", bytes);
    return buf;
}

void heapmap_print(FILE *out, favo_heap *heap, int width, int with_blocks)
{
    size_t na = favo_heap_arenas(heap, NULL, 0);
    size_t nb = favo_heap_snapshot(heap, NULL, 0);
    favo_arena_info *arenas = (favo_arena_info *)calloc(na ? na : 1, sizeof(favo_arena_info));
    favo_block_info *blocks = (favo_block_info *)calloc(nb ? nb : 1, sizeof(favo_block_info));
    if (!arenas || !blocks) {
        free(arenas);
        free(blocks);
        return;
    }
    na = favo_heap_arenas(heap, arenas, na);
    nb = favo_heap_snapshot(heap, blocks, nb);
    char *row = (char *)malloc((size_t)width + 1);
    char b1[32];
    for (size_t a = 0; a < na; a++) {
        memset(row, ' ', (size_t)width);
        row[width] = '\0';
        size_t span = arenas[a].end_offset - arenas[a].first_offset;
        for (size_t i = 0; i < nb; i++) {
            if (blocks[i].arena != a)
                continue;
            size_t from = (blocks[i].offset - arenas[a].first_offset) * (size_t)width / span;
            size_t to = (blocks[i].offset - arenas[a].first_offset + blocks[i].size) * (size_t)width / span;
            if (to <= from)
                to = from + 1;
            char mark = blocks[i].large ? '=' : (blocks[i].used ? '#' : '.');
            for (size_t c = from; c < to && c < (size_t)width; c++)
                row[c] = mark;
            if (blocks[i].used && from < (size_t)width && to - from >= 3)
                row[from] = '[';
        }
        fprintf(out, "arena %zu  %-5s %10s  |%s|\n", a, arenas[a].large ? "grande" : (arenas[a].source == FAVO_SOURCE_SBRK ? "sbrk" : "mmap"),
            human(arenas[a].length, b1, sizeof(b1)), row);
    }
    fprintf(out, "legenda: # ocupado  . livre  = bloco grande (mmap proprio)\n");
    if (with_blocks) {
        fprintf(out, "\n%-6s %-8s %-10s %-10s %-10s %s\n", "arena", "estado", "deslocam.", "bloco", "pedido", "endereco");
        for (size_t i = 0; i < nb; i++) {
            fprintf(out, "%-6zu %-8s %-10zu %-10zu %-10zu 0x%llx\n", blocks[i].arena,
                blocks[i].large ? "grande" : (blocks[i].used ? "ocupado" : "livre"), blocks[i].offset, blocks[i].size,
                blocks[i].request, (unsigned long long)blocks[i].address);
        }
    }
    free(row);
    free(arenas);
    free(blocks);
}

void stats_print(FILE *out, favo_heap *heap)
{
    favo_stats s;
    char b1[32];
    char b2[32];
    char b3[32];
    favo_heap_stats(heap, &s);
    fprintf(out, "politica          %s\n", favo_policy_name(favo_heap_get_policy(heap)));
    fprintf(out, "arenas            %zu (%s, pico %s)\n", s.arena_count, human(s.arena_bytes, b1, sizeof(b1)), human(s.peak_arena_bytes, b2, sizeof(b2)));
    fprintf(out, "blocos ocupados   %zu (%s, pedidos %s)\n", s.used_blocks, human(s.used_bytes, b1, sizeof(b1)), human(s.requested_bytes, b3, sizeof(b3)));
    fprintf(out, "blocos livres     %zu (%s, maior %s)\n", s.free_blocks, human(s.free_bytes, b1, sizeof(b1)), human(s.largest_free, b2, sizeof(b2)));
    fprintf(out, "fragmentacao      %.1f%%\n", s.fragmentation * 100.0);
    fprintf(out, "aproveitamento    %.1f%%\n", s.utilization * 100.0);
    fprintf(out, "operacoes         %llu alocacoes, %llu liberacoes, %llu realocacoes, %llu falhas\n",
        (unsigned long long)s.total_allocs, (unsigned long long)s.total_frees, (unsigned long long)s.total_reallocs,
        (unsigned long long)s.failed_allocs);
    fprintf(out, "estrutura         %llu divisoes, %llu fusoes, %llu passos de busca\n", (unsigned long long)s.splits,
        (unsigned long long)s.coalesces, (unsigned long long)s.search_steps);
    fprintf(out, "sistema           %llu pedidos, %llu devolucoes\n", (unsigned long long)s.os_requests, (unsigned long long)s.os_releases);
}
