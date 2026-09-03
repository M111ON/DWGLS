/* tools/tesspack_graft.c — .tesspack → valid GGUF → llama.cpp inference
 * ═══════════════════════════════════════════════════════════════════════════
 * Reads MoE expert tensors from .tesspack, grafts them into original GGUF.
 * Output is a valid GGUF that llama.cpp can load directly.
 *
 * BUILD: make tess-graft
 * RUN:   ./build/tesspack_graft [gguf_path] [tesspack_path] [graft_path]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

#define GGUF_ALIGN 32u
static inline uint64_t align32(uint64_t x) { return (x + GGUF_ALIGN - 1) & ~(uint64_t)(GGUF_ALIGN - 1); }

static int extract_layer(const char *name) {
    const char *p = strstr(name, "blk.");
    if (!p) return -1;
    return atoi(p + 4);
}

static int is_moe_expert(const char *name) {
    return strstr(name, "_exps.weight") != NULL;
}

/* ═══════════════ LOAD FULL TENSOR FROM .tesspack ═══════════════ */

static int load_tensor_from_pack(TESS_PackIndex *pi, const char *tensor_name,
                                 uint32_t n_capos, uint32_t total_cells,
                                 uint32_t cell_size, uint8_t *out_buf) {
    for (uint32_t c = 0; c < n_capos; c++) {
        TESS_CapoReader cr;
        int rc = tess_pack_get_capo(pi, &cr, tensor_name, c);
        if (rc != 0) {
            fprintf(stderr, "  FAIL: pack_get_capo(%s, capo=%u) rc=%d\n", tensor_name, c, rc);
            return -1;
        }
        uint32_t cells_in_capo = (c < n_capos - 1) ? TESS_TOTAL_SLOTS
                              : (total_cells - c * TESS_TOTAL_SLOTS);
        if (tess_capo_load_range(&cr, 0, cells_in_capo, out_buf + (uint64_t)c * TESS_TOTAL_SLOTS * cell_size) == 0) {
            fprintf(stderr, "  FAIL: capo_load_range(%s, capo=%u)\n", tensor_name, c);
            return -1;
        }
    }
    return 0;
}

/* ═══════════════ MAIN ═══════════════ */

int main(int argc, char **argv) {
    const char *gguf_path    = (argc > 1) ? argv[1] : "F:\\model\\qwen3-4b-moe-q4_k_m.gguf";
    const char *pack_path    = (argc > 2) ? argv[2] : "qwen3moe.tesspack";
    const char *graft_path   = (argc > 3) ? argv[3] : "F:/model/moe_tesspack_graft.gguf";

    printf("=== Tesspack Graft: .tesspack → GGUF → inference ===\n");
    printf("GGUF:      %s\n", gguf_path);
    printf("Pack:      %s\n", pack_path);
    printf("Graft:     %s\n", graft_path);

    /* open GGUF */
    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        printf("FAIL: cannot open GGUF\n");
        return 1;
    }
    printf("Tensors:   %u\n", gguf.n_tensors);

    /* open .tesspack */
    TESS_PackIndex pi;
    if (tess_pack_open(&pi, pack_path) != 0) {
        printf("FAIL: cannot open .tesspack\n");
        gguf_close(&gguf);
        return 1;
    }
    printf("Pack capos: %u\n", pi.n_entries);

    /* match MoE tensors in GGUF against pack entries */
    uint32_t n_match = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (!is_moe_expert(gguf.names[i])) continue;
        /* find in pack */
        int found = 0;
        for (uint32_t j = 0; j < pi.n_entries; j++) {
            if (strcmp(pi.entries[j].name, gguf.names[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (found) n_match++;
        else fprintf(stderr, "  WARN: %s not in pack\n", gguf.names[i]);
    }
    printf("Matched:   %u MoE tensors in pack\n", n_match);

    /* build body: same layout as source GGUF */
    size_t hdr_sz = (size_t)gguf.data_offset;
    size_t body_sz = (size_t)(gguf.base_sz - gguf.data_offset);
    uint8_t *body = (uint8_t *)calloc(1, body_sz);
    if (!body) { printf("FAIL: OOM\n"); return 1; }

    uint32_t from_pack = 0, from_source = 0, skipped = 0;
    uint64_t pack_bytes = 0;

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        uint64_t off = gguf.offsets[i];
        uint32_t tsz = gguf.sizes[i];
        uint32_t csz = GGUF_CELL_SIZE[gguf.dtypes[i]];
        if (csz == 0) csz = 1;

        if (is_moe_expert(gguf.names[i])) {
            /* find in pack */
            int capo_count = 0;
            uint32_t capo_total_cells = 0;
            for (uint32_t j = 0; j < pi.n_entries; j++) {
                if (strcmp(pi.entries[j].name, gguf.names[i]) == 0) {
                    capo_total_cells += TESS_TOTAL_SLOTS;
                    if (pi.entries[j].capo_id + 1 > (uint32_t)capo_count)
                        capo_count = pi.entries[j].capo_id + 1;
                }
            }

            if (capo_count > 0) {
                uint32_t total_cells = tsz / csz;
                if (capo_total_cells < total_cells) capo_total_cells = total_cells;

                uint8_t *tdata = (uint8_t *)malloc((size_t)capo_total_cells * csz + 64);
                if (!tdata) { skipped++; continue; }

                if (load_tensor_from_pack(&pi, gguf.names[i], capo_count,
                                          total_cells, csz, tdata) == 0) {
                    if (off + tsz <= body_sz) {
                        memcpy(body + off, tdata, tsz);
                        from_pack++;
                        pack_bytes += tsz;
                        printf("  PACK  [%2d] %-48s  %8u bytes\n",
                               extract_layer(gguf.names[i]), gguf.names[i], tsz);
                    } else {
                        skipped++;
                    }
                } else {
                    fprintf(stderr, "  FAIL  [%2d] %s\n",
                            extract_layer(gguf.names[i]), gguf.names[i]);
                    skipped++;
                }
                free(tdata);
            } else {
                skipped++;
            }
        } else {
            /* non-MoE tensor: copy from source */
            uint64_t src_off = gguf.data_offset + gguf.offsets[i];
            if (src_off + tsz <= gguf.base_sz) {
                memcpy(body + off, gguf.base + src_off, tsz);
                from_source++;
            }
        }
    }

    printf("  from pack: %u tensors (%.1f MB)\n",
           from_pack, pack_bytes / 1e6);
    printf("  from source: %u tensors\n", from_source);
    printf("  skipped: %u\n", skipped);

    /* write graft GGUF */
    FILE *f = fopen(graft_path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", graft_path); free(body); return 1; }

    if (fwrite(gguf.base, 1, hdr_sz, f) != hdr_sz ||
        fwrite(body, 1, body_sz, f) != body_sz) {
        printf("FAIL: write error\n");
        fclose(f); free(body); return 1;
    }
    fclose(f);
    printf("Written:   %s (%.1f MB)\n", graft_path,
           (double)(hdr_sz + body_sz) / 1e6);

    free(body);
    tess_pack_close(&pi);
    gguf_close(&gguf);
    return 0;
}
