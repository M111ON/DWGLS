/* tools/tess_moe_bridge.c — GGUF → .tess multi-capo → MoE expert serve pipeline
 *
 * Pipeline: GGUF → scatter-encode (multi-capo files on disk) → stream-serve
 * individual expert blocks → verify lossless roundtrip.
 *
 * Multi-capo: tensor with N cells → ceil(N/20736) capo files on disk.
 * Expert E has blocks [E*bpe, (E+1)*bpe) which may span capo boundaries.
 *
 * BUILD: gcc -O2 -Wall -I core -I core/infra -o tess_moe_bridge tools/tess_moe_bridge.c -lm
 * RUN:   ./tess_moe_bridge <model.gguf> [tess_dir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

typedef struct { const char *substr; int wtype; } TensorPattern;
static const TensorPattern PATTERNS[] = {
    {".ffn_down_exps.weight",  0},
    {".ffn_gate_exps.weight",  1},
    {".ffn_up_exps.weight",    2},
};
#define N_PATTERNS (sizeof(PATTERNS)/sizeof(PATTERNS[0]))

static int extract_layer(const char *name) {
    const char *p = strstr(name, "blk.");
    if (!p) return -1;
    return atoi(p + 4);
}

static int match_tensor(const char *name, int *out_layer, int *out_wtype) {
    int layer = extract_layer(name);
    if (layer < 0) return 0;
    for (size_t i = 0; i < N_PATTERNS; i++) {
        if (strstr(name, PATTERNS[i].substr)) {
            *out_layer = layer;
            *out_wtype = PATTERNS[i].wtype;
            return 1;
        }
    }
    return 0;
}

/* scatter-encode one capo chunk into a .tess buffer, write to disk */
static int bake_one_capo(const void *src, uint32_t n_elems, uint32_t cell_size,
                          uint32_t gguf_type, uint32_t capo_id, uint32_t capo_total,
                          const char *tess_dir, const char *tensor_name) {
    uint32_t cube_bytes = TESS_TOTAL_SLOTS * cell_size;
    uint32_t payload_size = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;

    uint8_t *buf = (uint8_t *)malloc(payload_size);
    if (!buf) return -1;

    uint8_t *p = buf;

    TESS_Header hdr;
    tess_header_init(&hdr, gguf_type, cell_size);
    hdr.scale_factor = 65536u;
    hdr.x_slots = TESS_X_SLOTS;
    hdr.y_slots = TESS_Y_SLOTS;
    hdr.z_slots = TESS_Z_SLOTS;
    hdr.tensor_count = n_elems;
    memcpy(p, &hdr, TESS_HEADER_SIZE);
    p += TESS_HEADER_SIZE;

    TESS_Formula fml;
    tess_formula_init(&fml);
    fml.mirror_axis_x = hdr.x_slots;
    fml.mirror_axis_y = hdr.y_slots;
    fml.mirror_axis_z = hdr.z_slots;
    fml.stride_seed = TESS_STRIDE_37;
    fml.capo_id = capo_id;
    fml.capo_total = (uint8_t)capo_total;
    memcpy(p, &fml, TESS_FORMULA_SIZE);
    p += TESS_FORMULA_SIZE;

    uint8_t *cube_data = p;
    memset(cube_data, 0, cube_bytes);
    const uint8_t *src_bytes = (const uint8_t *)src;
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = tess_stride_scatter(i);
        if (slot >= TESS_TOTAL_SLOTS) slot = i % TESS_TOTAL_SLOTS;
        uint32_t off = slot * cell_size;
        if (off + cell_size <= cube_bytes)
            memcpy(cube_data + off, src_bytes + (uint64_t)i * cell_size, cell_size);
    }
    p += cube_bytes;

    uint64_t cube_crc = tess_crc64(cube_data, cube_bytes);
    memcpy(p, &cube_crc, TESS_CRC_SIZE);
    p += TESS_CRC_SIZE;
    ((TESS_Header *)buf)->cube_checksum = cube_crc;

    /* write to disk */
    char path[1024];
    if (capo_total == 1)
        snprintf(path, sizeof(path), "%s/%s.tess", tess_dir, tensor_name);
    else
        snprintf(path, sizeof(path), "%s/%s_capo%u.tess", tess_dir, tensor_name, capo_id);

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(buf, 1, (size_t)(p - buf), f);
    fclose(f);
    free(buf);
    return 0;
}

/* bake entire tensor into multi-capo files on disk */
static int bake_tensor(const void *src, uint32_t total_cells, uint32_t cell_size,
                        uint32_t gguf_type, const char *tess_dir, const char *tensor_name) {
    uint32_t capo_total = (total_cells + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;
    const uint8_t *src_bytes = (const uint8_t *)src;

    for (uint32_t c = 0; c < capo_total; c++) {
        uint32_t offset = c * TESS_TOTAL_SLOTS;
        uint32_t chunk = total_cells - offset;
        if (chunk > TESS_TOTAL_SLOTS) chunk = TESS_TOTAL_SLOTS;
        if (bake_one_capo(src_bytes + (uint64_t)offset * cell_size,
                          chunk, cell_size, gguf_type,
                          c, capo_total, tess_dir, tensor_name) != 0)
            return -1;
    }
    return 0;
}

/* stream-serve: load a range of cells from multi-capo files on disk */
static int stream_load_range(const char *tess_dir, const char *tensor_name,
                              uint32_t cell_size, uint32_t total_cells,
                              uint32_t start_cell, uint32_t count, uint8_t *dst) {
    for (uint32_t cell = start_cell; cell < start_cell + count; ) {
        uint32_t capo_id = cell / TESS_TOTAL_SLOTS;
        uint32_t capo_local = cell % TESS_TOTAL_SLOTS;
        uint32_t capo_end = (capo_id + 1) * TESS_TOTAL_SLOTS;
        uint32_t chunk = start_cell + count - cell;
        if (chunk > capo_end - cell) chunk = capo_end - cell;

        char path[1024];
        uint32_t capo_total = (total_cells + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;
        if (capo_total == 1)
            snprintf(path, sizeof(path), "%s/%s.tess", tess_dir, tensor_name);
        else
            snprintf(path, sizeof(path), "%s/%s_capo%u.tess", tess_dir, tensor_name, capo_id);

        TESS_CapoReader r;
        if (tess_capo_open(&r, path) != 0) return -1;
        int n = tess_capo_load_range(&r, capo_local, chunk, dst);
        tess_capo_close(&r);
        if (n <= 0) return -1;

        dst += (uint64_t)chunk * cell_size;
        cell += chunk;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] : "F:/model/qwen3-4b-moe-q4_k_m.gguf";
    const char *tess_dir  = (argc > 2) ? argv[2] : "build/tess_moe_out";

    printf("=== .tess ↔ MoE Bridge Pipeline ===\n");
    printf("GGUF:   %s\n", gguf_path);
    printf("tess_dir: %s\n", tess_dir);

    /* create output directory */
    mkdir(tess_dir);

    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        printf("FAIL: cannot open GGUF\n");
        return 1;
    }
    printf("Tensors: %u\n", gguf.n_tensors);

    /* pass 1: count MoE tensors per wtype, find n_layers and n_experts */
    uint32_t n_match = 0, max_layer = 0, n_wtypes[3] = {0};
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (match_tensor(gguf.names[i], &layer, &wtype)) {
            n_match++;
            if ((uint32_t)layer > max_layer) max_layer = (uint32_t)layer;
            if (wtype >= 0 && wtype < 3) n_wtypes[wtype]++;
        }
    }
    uint32_t n_layers = max_layer + 1;
    printf("MoE tensors: %u (%u layers × 3 wtypes)\n", n_match, n_layers);

    if (n_match == 0) {
        printf("No MoE tensors found.\n");
        gguf_close(&gguf);
        return 1;
    }

    uint32_t total_pass = 0, total_fail = 0;

    /* pass 2: for each wtype, bake → stream → verify */
    for (int wtype = 0; wtype < 3; wtype++) {
        uint32_t n_for_wtype = n_wtypes[wtype];
        if (n_for_wtype == 0) continue;

        const char *wname = wtype == 0 ? "down" : wtype == 1 ? "gate" : "up";
        printf("\n── wtype %d (%s): %u layers ──\n", wtype, wname, n_for_wtype);

        for (uint32_t li = 0; li < n_layers; li++) {
            /* find the tensor */
            uint32_t found_idx = UINT32_MAX;
            for (uint32_t i = 0; i < gguf.n_tensors; i++) {
                int layer, wt;
                if (match_tensor(gguf.names[i], &layer, &wt) && layer == (int)li && wt == wtype) {
                    found_idx = i;
                    break;
                }
            }
            if (found_idx == UINT32_MAX) { total_fail++; continue; }

            uint32_t tsz = gguf.sizes[found_idx];
            uint32_t cell_sz = GGUF_CELL_SIZE[gguf.dtypes[found_idx]];
            if (cell_sz == 0) { total_fail++; continue; }
            uint32_t total_cells = tsz / cell_sz;
            /* n_experts from tensor dims: last dim = n_experts */
            uint32_t n_experts = 1;
            if (gguf.n_dims[found_idx] >= 2) {
                n_experts = gguf.dims[found_idx * 4 + gguf.n_dims[found_idx] - 1];
            }
            uint32_t bpe = (n_experts > 0) ? total_cells / n_experts : 0;

            /* load raw tensor data from GGUF */
            uint8_t *tensor_data = (uint8_t *)malloc(tsz);
            if (gguf_read_tensor(gguf_path, &gguf, found_idx, tensor_data, tsz) != 0) {
                printf("  FAIL: read L%u\n", li);
                free(tensor_data); total_fail++; continue;
            }

            /* bake: GGUF → multi-capo .tess files on disk */
            if (bake_tensor(tensor_data, total_cells, cell_sz,
                            gguf.dtypes[found_idx], tess_dir, gguf.names[found_idx]) != 0) {
                printf("  FAIL: bake L%u\n", li);
                free(tensor_data); total_fail++; continue;
            }

            /* stream-serve each expert and verify against original GGUF data */
            for (uint32_t ei = 0; ei < n_experts; ei++) {
                uint32_t expert_cells = bpe;
                uint8_t *served = (uint8_t *)malloc((uint64_t)expert_cells * cell_sz);
                uint8_t *orig   = tensor_data + (uint64_t)ei * bpe * cell_sz;

                if (stream_load_range(tess_dir, gguf.names[found_idx],
                                      cell_sz, total_cells,
                                      ei * bpe, expert_cells, served) != 0) {
                    printf("  FAIL: stream L%u E%u\n", li, ei);
                    free(served); total_fail++; continue;
                }

                if (memcmp(served, orig, (uint64_t)expert_cells * cell_sz) == 0) {
                    total_pass++;
                } else {
                    printf("  MISMATCH: L%u E%u\n", li, ei);
                    total_fail++;
                }
                free(served);
            }

            free(tensor_data);
        }
    }

    printf("\n=== SUMMARY ===\n");
    printf("PASS: %u  FAIL: %u\n", total_pass, total_fail);
    int pass = (total_fail == 0 && total_pass > 0);
    printf("GATE: %s\n", pass ? "PASS" : "FAIL");

    gguf_close(&gguf);
    return pass ? 0 : 1;
}
