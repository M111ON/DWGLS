/* tess_verify_tensor.c — Compare raw tensor bytes: GGUF direct vs tesspack roundtrip.
   Usage: tess_verify_tensor <model.gguf> <model.tesspack> <tensor_name> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/gguf_reader.h"
#include "core/geo_tess_container.h"

static uint32_t cell_size_of(uint32_t dtype) {
    static const uint32_t C[] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    if (dtype < 16) return C[dtype];
    if (dtype == 41) return 6;
    return 0;
}

static int load_from_pack(TESS_PackIndex *pi, const char *name, uint32_t cell_size,
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
        uint32_t cells = (cells_left >= TESS_TOTAL_SLOTS) ? TESS_TOTAL_SLOTS : (uint32_t)cells_left;
        uint8_t *dst_c = dst + (uint64_t)c * TESS_TOTAL_SLOTS * cell_size;
        uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells, dst_c);
        if (got != cells * cell_size) return -3;
        cells_left -= cells;
    }
    return (cells_left == 0) ? (int)capo_count : -4;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <model.gguf> <model.tesspack> <tensor_name>\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *pack_path = argv[2];
    const char *target = argv[3];

    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        fprintf(stderr, "FAIL: gguf_open\n"); return 1;
    }
    fprintf(stderr, "GGUF: %u tensors, data_offset=%lu\n", gguf.n_tensors, (unsigned long)gguf.data_offset);

    /* Find target */
    int ti = -1;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (strcmp(gguf.names[i], target) == 0) { ti = i; break; }
    }
    if (ti < 0) { fprintf(stderr, "Tensor '%s' not found\n", target); return 1; }

    uint32_t dtype = gguf.dtypes[ti];
    uint32_t csz = cell_size_of(dtype);
    uint64_t tensor_off = gguf.offsets[ti];
    uint32_t tensor_sz_bytes = gguf.sizes[ti];  /* gguf.sizes = byte count */
    uint64_t n_blocks = (csz > 0) ? tensor_sz_bytes / csz : 0;

    fprintf(stderr, "Tensor '%s': dtype=%u csz=%u tensor_bytes=%u n_blocks=%lu\n",
            target, dtype, csz, tensor_sz_bytes, (unsigned long)n_blocks);

    if (csz == 0 || n_blocks == 0) { fprintf(stderr, "csz=0 or n_blocks=0\n"); return 1; }

    /* Raw GGUF data */
    uint8_t *gguf_data = (uint8_t *)malloc(tensor_sz_bytes);
    memcpy(gguf_data, gguf.base + gguf.data_offset + tensor_off, tensor_sz_bytes);

    /* Pack data */
    TESS_PackIndex pi;
    if (tess_pack_open_mmap(&pi, pack_path) != 0) {
        fprintf(stderr, "FAIL: pack_open\n"); free(gguf_data); return 1;
    }
    fprintf(stderr, "Pack: %u capos\n", pi.n_capos);

    uint8_t *pack_data = (uint8_t *)calloc(1, tensor_sz_bytes + 16);
    int rc = load_from_pack(&pi, target, csz, n_blocks, pack_data);
    fprintf(stderr, "Pack load: rc=%d capos\n", rc);
    if (rc < 0) { free(gguf_data); free(pack_data); return 1; }

    /* Compare */
    fprintf(stderr, "\nComparing %u bytes of '%s'...\n", tensor_sz_bytes, target);
    int diffs = 0;
    for (uint64_t i = 0; i < tensor_sz_bytes; i++) {
        if (gguf_data[i] != pack_data[i]) {
            if (diffs < 20)
                fprintf(stderr, "  DIFF [%lu]: gguf=0x%02x pack=0x%02x\n",
                        (unsigned long)i, gguf_data[i], pack_data[i]);
            diffs++;
        }
    }

    if (diffs == 0) {
        fprintf(stderr, "IDENTICAL\n");
    } else {
        fprintf(stderr, "MISMATCH: %d / %u bytes differ (%.4f%%)\n",
                diffs, tensor_sz_bytes, 100.0 * diffs / tensor_sz_bytes);
    }

    free(gguf_data); free(pack_data);
    return diffs > 0 ? 1 : 0;
}
