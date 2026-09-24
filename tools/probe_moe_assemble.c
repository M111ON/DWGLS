/* probe_moe_assemble.c — replicate tesspack_server assemble decisions per tensor.
 * Prints every tensor with rc<=0 plus summary. Uses real gguf_reader + container. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/gguf_reader.h"
#include "core/geo_tess_container.h"

static uint32_t cell_size_of(uint32_t t) {
    static const uint32_t C[] = {4,2,18,20,0,0,22,24,34,36,84,110,144,176,210,292};
    if (t < 16) return C[t];
    if (t == 41) return 6;
    return 0;
}

/* verbatim copy of server load_pack_tensor (returns capo_count or negative) */
static int load_pack_tensor(TESS_PackIndex *pi, const char *name, uint32_t cell_size,
                            uint64_t total_cells, uint8_t *dst) {
    uint32_t capo_count = 0;
    {
        const uint8_t *cur = pi->base + pi->index_offset;
        const uint8_t *end = pi->base + pi->file_sz;
        uint32_t nlen = (uint32_t)strlen(name);
        for (uint32_t i = 0; i < pi->n_capos; i++) {
            if (cur + 1 > end) break;
            uint8_t nl = *cur++;
            if (cur + nl + 16 > end) break;
            const uint8_t *np = cur;
            cur += nl;
            uint32_t cid = *(const uint32_t *)cur;
            cur += 16;
            if (nl == (uint8_t)nlen && memcmp(np, name, nlen) == 0) {
                if (cid + 1 > capo_count) capo_count = cid + 1;
            }
        }
    }
    if (capo_count == 0) return -1;
    uint64_t cells_left = total_cells;
    for (uint32_t c = 0; c < capo_count && cells_left > 0; c++) {
        TESS_CapoReader cr;
        if (tess_pack_get_capo_mmap(pi, &cr, name, c) != 0) return -2;
        uint32_t cells = (cells_left >= TESS_TOTAL_SLOTS)
                       ? TESS_TOTAL_SLOTS : (uint32_t)cells_left;
        uint8_t *dst_c = dst + (uint64_t)c * TESS_TOTAL_SLOTS * cell_size;
        uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells, dst_c);
        if (got != cells * cell_size) return -3;
        cells_left -= cells;
    }
    return (cells_left == 0) ? (int)capo_count : -4;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: %s <model.gguf> <model.tesspack>\n", argv[0]); return 1; }
    GgufReader gr;
    if (gguf_open(argv[1], &gr) != 0) { fprintf(stderr, "gguf open fail\n"); return 1; }
    TESS_PackIndex pi;
    if (tess_pack_open_mmap(&pi, argv[2]) != 0) { fprintf(stderr, "pack open fail\n"); return 1; }
    uint32_t N = gr.n_tensors;
    size_t maxsz = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint64_t sz = gr.sizes[i];
        if (sz > maxsz) maxsz = (size_t)sz;
    }
    uint8_t *buf = (uint8_t *)malloc(maxsz ? maxsz : 1);
    int n_ok = 0, n_bad = 0, n_skip = 0;
    for (uint32_t i = 0; i < N; i++) {
        const char *name = gr.names[i];
        uint32_t ty = gr.dtypes[i];
        uint64_t tsz = gr.sizes[i];
        uint32_t csz = cell_size_of(ty);
        if (csz == 0) { printf("SKIP csz=0 ty=%u %s\n", ty, name); n_skip++; continue; }
        uint64_t total_cells = tsz / csz;
        if (total_cells * csz != tsz || total_cells == 0) {
            printf("SKIP indivisible %s tsz=%I64u csz=%u\n", name,
                   (unsigned long long)tsz, csz); n_skip++; continue;
        }
        int rc = load_pack_tensor(&pi, name, csz, total_cells, buf);
        if (rc > 0) n_ok++;
        else { printf("FAIL rc=%d ty=%u tsz=%I64u %s\n", rc, ty,
                      (unsigned long long)tsz, name); n_bad++; }
    }
    printf("SUMMARY ok=%d bad=%d skip=%d total=%u\n", n_ok, n_bad, n_skip, N);
    return 0;
}
