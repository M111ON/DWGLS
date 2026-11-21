/* test_cube_wire.c — Wire proof: multi-cube resolver on real .tesspack.
 * Opens a .tesspack, gets TessMultiCubeView (zero-copy), uses tess_resolve_multi
 * to read weights, verifies against GGUF.
 *
 * Usage: test_cube_wire <model.gguf> <model.tesspack> <tensor_name>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"
#include "../core/tess_resolver.h"

static uint32_t cell_size_of(uint32_t dtype) {
    static const uint32_t C[] = { 4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292 };
    if (dtype < 16) return C[dtype];
    if (dtype == 41) return 6;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Usage: %s <gguf> <tesspack> <tensor>\n", argv[0]); return 1; }

    GgufReader gguf;
    if (gguf_open(argv[1], &gguf) != 0) { fprintf(stderr, "FAIL: gguf_open\n"); return 1; }

    int ti = -1;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (strcmp(gguf.names[i], argv[3]) == 0) { ti = (int)i; break; }
    }
    if (ti < 0) { fprintf(stderr, "Tensor '%s' not found\n", argv[3]); return 1; }

    uint32_t dtype = gguf.dtypes[ti];
    uint32_t csz = cell_size_of(dtype);
    uint64_t tensor_off = gguf.offsets[ti];
    uint32_t tensor_sz = gguf.sizes[ti];
    uint64_t n_blocks = (csz > 0) ? tensor_sz / csz : 0;

    fprintf(stderr, "Tensor '%s': dtype=%u csz=%u bytes=%u blocks=%lu\n",
            argv[3], dtype, csz, tensor_sz, (unsigned long)n_blocks);

    TESS_PackIndex pi;
    if (tess_pack_open_mmap(&pi, argv[2]) != 0) { fprintf(stderr, "FAIL: pack_open\n"); return 1; }
    fprintf(stderr, "Pack: %u capos, %.1f MB\n", pi.n_capos, (double)pi.file_sz / 1e6);

    /* Count capos for this tensor */
    uint32_t capo_count = 0;
    {
        const uint8_t *cur = pi.base + pi.index_offset;
        const uint8_t *end = pi.base + pi.file_sz;
        uint32_t nlen = (uint32_t)strlen(argv[3]);
        for (uint32_t j = 0; j < pi.n_capos; j++) {
            if (cur + 1 > end) break;
            uint8_t nl = *cur++;
            if (cur + nl + 16 > end) break;
            const uint8_t *np = cur;
            cur += nl;
            uint32_t cid = *(const uint32_t *)cur;
            cur += 16;
            if (nl == (uint8_t)nlen && memcmp(np, argv[3], nlen) == 0) {
                if (cid + 1 > capo_count) capo_count = cid + 1;
            }
        }
    }
    if (capo_count == 0) capo_count = 1;
    fprintf(stderr, "Capos for '%s': %u\n", argv[3], capo_count);

    /* Build multi-cube view (zero-copy) */
    TessMultiCubeView mc;
    mc.n_caps = capo_count;
    mc.cell_size = csz;
    mc.total_cells = n_blocks;
    mc.caps = (TessCubeView *)calloc(capo_count, sizeof(TessCubeView));
    for (uint32_t c = 0; c < capo_count; c++) {
        TESS_CapoReader cr;
        if (tess_pack_get_capo_mmap(&pi, &cr, argv[3], c) != 0) {
            mc.caps[c].base = NULL;
            mc.caps[c].cell_size = csz;
            mc.caps[c].n_cells = TESS_TOTAL_SLOTS;
            mc.caps[c].capo_id = c;
            continue;
        }
        mc.caps[c].base = cr.cube_data;
        mc.caps[c].cell_size = cr.cell_size;
        mc.caps[c].n_cells = cr.n_elems;
        mc.caps[c].capo_id = c;
    }
    fprintf(stderr, "Multi-cube view: %u capos (zero-copy)\n", mc.n_caps);

    /* Resolve all weights (zero-copy pointer arithmetic) */
    uint8_t *resolved = (uint8_t *)calloc(tensor_sz, 1);
    int null_count = 0;
    for (uint64_t s = 0; s < n_blocks; s++) {
        const uint8_t *p = tess_resolve_multi(&mc, s);
        if (!p) { null_count++; continue; }
        memcpy(resolved + s * csz, p, csz);
    }
    if (null_count > 0) fprintf(stderr, "  WARNING: %d null resolves\n", null_count);

    /* Compare with GGUF */
    uint8_t *gguf_data = (uint8_t *)malloc(tensor_sz);
    memcpy(gguf_data, gguf.base + gguf.data_offset + tensor_off, tensor_sz);

    int diffs = 0;
    for (uint64_t i = 0; i < tensor_sz; i++) {
        if (gguf_data[i] != resolved[i]) {
            if (diffs < 5) fprintf(stderr, "  DIFF [%lu]: gguf=0x%02x resolved=0x%02x\n", (unsigned long)i, gguf_data[i], resolved[i]);
            diffs++;
        }
    }

    if (diffs == 0) {
        fprintf(stderr, "RESULT: IDENTICAL (zero-copy multi-cube resolve == GGUF)\n");
    } else {
        fprintf(stderr, "RESULT: MISMATCH %d/%u bytes\n", diffs, tensor_sz);
    }

    free(mc.caps); free(gguf_data); free(resolved);
    tess_pack_close(&pi);
    return diffs > 0 ? 1 : 0;
}
