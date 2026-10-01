/* tools/moe_route_dry.c — MoE router-join dry run (server join decision table).
 * ═══════════════════════════════════════════════════════════════════════════
 * Reads router gates (blk.L.ffn_gate_inp.weight) from a .tesspack, scores
 * experts per layer with the column-sum heuristic (same as moe_expert_route.c),
 * and prints the top-K table the server join needs: which (layer, expert)
 * to decode vs skip, with pack-presence per expert tensor.
 *
 * No inference, no llama load, no rebuild. Gate tensors are decoded;
 * expert presence is index-only (no capo decode).
 *
 * HONEST LIMIT: col-sum scoring is input-independent static heuristic, NOT
 * per-token routing. True per-token join needs the callback path (see #803).
 *
 * BUILD: make moe-route-dry
 * RUN:   ./build/moe_route_dry <model.gguf> <model.tesspack> [topk=4]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "core/gguf_reader.h"
#include "core/geo_tess_container.h"
#include "core/moe_expert_addr.h"

/* ── verbatim load path from probe_moe_assemble.c (gate decode only) ── */
static int load_pack_tensor(TESS_PackIndex *pi, const char *name, uint32_t cell_size,
                            uint64_t total_cells, uint8_t *dst) {
    const uint8_t *onion = NULL;
    uint32_t onion_sz = 0;
    if (tess_pack_find_onion(pi, name, &onion, &onion_sz) == 0) {
        uint64_t need = total_cells * (uint64_t)cell_size;
        if (onion_sz == need) { memcpy(dst, onion, onion_sz); return 1; }
        return -5;
    }
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

/* index-only presence (no decode) */
static int pack_has_tensor(TESS_PackIndex *pi, const char *name) {
    const uint8_t *onion = NULL;
    uint32_t onion_sz = 0;
    if (tess_pack_find_onion(pi, name, &onion, &onion_sz) == 0) return 1;
    const uint8_t *cur = pi->base + pi->index_offset;
    const uint8_t *end = pi->base + pi->file_sz;
    uint32_t nlen = (uint32_t)strlen(name);
    for (uint32_t i = 0; i < pi->n_capos; i++) {
        if (cur + 1 > end) break;
        uint8_t nl = *cur++;
        if (cur + nl + 16 > end) break;
        if (nl == (uint8_t)nlen && memcmp(cur, name, nlen) == 0) return 1;
        cur += nl + 16;
    }
    return 0;
}

/* fp16→fp32 from moe_expert_route.c (strict-aliasing safe: caller passes u16) */
static float fp16_to_fp32(uint16_t h) {
    int sign = (h >> 15) & 1;
    int exp  = ((h >> 10) & 0x1f) - 15;
    int mant = h & 0x3ff;
    if (exp == -15 && mant == 0) return sign ? -0.0f : 0.0f;
    if (exp == 16 && mant == 0) return sign ? -INFINITY : INFINITY;
    if (exp == 16 && mant != 0) return NAN;
    float v;
    if (exp == -15) v = (float)mant / 1024.0f;
    else v = (1.0f + (float)mant / 1024.0f) * ldexpf(1.0f, exp);
    return sign ? -v : v;
}

static void topk(const float *vals, int n, int k, int *out) {
    int *idx = (int *)malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 0; i < k && i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++)
            if (vals[idx[j]] > vals[idx[best]]) best = j;
        int tmp = idx[i]; idx[i] = idx[best]; idx[best] = tmp;
    }
    memcpy(out, idx, (size_t)k * sizeof(int));
    free(idx);
}

static int find_tensor(GgufReader *gr, const char *name) {
    for (uint32_t i = 0; i < gr->n_tensors; i++)
        if (strcmp(gr->names[i], name) == 0) return (int)i;
    return -1;
}

static const char *EXPS_PAT[] = {
    ".ffn_down_exps.weight", ".ffn_gate_exps.weight", ".ffn_up_exps.weight",
};
#define N_EXPS_PAT 3

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <model.gguf> <model.tesspack> [topk=4]\n", argv[0]);
        return 1;
    }
    int TOPK = (argc > 3) ? atoi(argv[3]) : 4;
    if (TOPK < 1) TOPK = 1;
    if (TOPK > 64) TOPK = 64;

    GgufReader gr;
    if (gguf_open(argv[1], &gr) != 0) { fprintf(stderr, "gguf open fail\n"); return 1; }
    TESS_PackIndex pi;
    if (tess_pack_open_mmap(&pi, argv[2]) != 0) { fprintf(stderr, "pack open fail\n"); return 1; }

    /* discover MoE layers from gate names (exact match: sscanf returns 1
     * even when the trailing literal mismatches, so anchor with %n) */
    int max_layer = -1;
    for (uint32_t i = 0; i < gr.n_tensors; i++) {
        int L = -1, nch = -1;
        if (sscanf(gr.names[i], "blk.%d.ffn_gate_inp.weight%n", &L, &nch) >= 1 &&
            nch >= 0 && gr.names[i][nch] == '\0' && L > max_layer)
            max_layer = L;
    }
    if (max_layer < 0) { printf("No ffn_gate_inp tensors — not a MoE model\n"); return 1; }
    int n_layers = max_layer + 1;
    printf("MoE layers: %d, top-K: %d (static col-sum heuristic)\n", n_layers, TOPK);

    uint64_t decode_bytes = 0, skip_bytes = 0;
    uint32_t decode_tensors = 0, skip_tensors = 0, gate_missing = 0;

    for (int L = 0; L < n_layers; L++) {
        char gate_name[128];
        snprintf(gate_name, sizeof(gate_name), "blk.%d.ffn_gate_inp.weight", L);
        int gi = find_tensor(&gr, gate_name);
        int n_exp = 0, gate_ok = 0;
        int routed[64] = {0};
        int k_eff = 0;

        if (gi >= 0) {
            uint32_t ty = gr.dtypes[gi];
            uint64_t tsz = gr.sizes[gi];
            int n_embd = (int)gr.dims[(size_t)gi * 4 + 0];
            n_exp = (int)gr.dims[(size_t)gi * 4 + 1];
            uint32_t csz = (ty == 0) ? 4 : (ty == 1) ? 2 : 0;
            if (n_embd > 0 && n_exp > 0 && csz > 0 &&
                tsz == (uint64_t)n_embd * (uint64_t)n_exp * csz) {
                uint8_t *buf = (uint8_t *)malloc((size_t)tsz);
                uint64_t cells = tsz / csz;
                int rc = load_pack_tensor(&pi, gate_name, csz, cells, buf);
                if (rc > 0) {
                    float *scores = (float *)malloc((size_t)n_exp * sizeof(float));
                    for (int e = 0; e < n_exp; e++) {
                        double sum = 0;
                        for (int j = 0; j < n_embd; j++) {
                            float v = (ty == 0)
                                ? ((const float *)buf)[(size_t)j * n_exp + e]
                                : fp16_to_fp32(((const uint16_t *)buf)[(size_t)j * n_exp + e]);
                            sum += v;
                        }
                        scores[e] = (float)sum;
                    }
                    k_eff = TOPK < n_exp ? TOPK : n_exp;
                    topk(scores, n_exp, k_eff, routed);
                    free(scores);
                    gate_ok = 1;
                }
                free(buf);
            }
        }
        if (!gate_ok) {
            gate_missing++;
            n_exp = 0;
            /* count experts from exps tensors present in reader */
            for (uint32_t i = 0; i < gr.n_tensors; i++) {
                int Li = -1, matched = 0;
                for (int p = 0; p < N_EXPS_PAT && !matched; p++) {
                    char want[160];
                    snprintf(want, sizeof(want), "blk.%d%s", L, EXPS_PAT[p]);
                    if (strcmp(gr.names[i], want) == 0) matched = 1;
                }
                if (matched && sscanf(gr.names[i], "blk.%d.", &Li) == 1 && Li == L) {
                    uint64_t tsz = gr.sizes[i];
                    /* stacked blob holds all experts; n_exp unknown without gate */
                    (void)tsz;
                }
            }
            for (int k = 0; k < TOPK; k++) routed[k] = k;
            k_eff = TOPK;
        }

        /* per-expert coverage: routed set × 3 wtypes */
        printf("L%02d gate=%s n_exp=%d top%d=[", L, gate_ok ? "pack" : "MISS",
               gate_ok ? n_exp : -1, k_eff);
        for (int k = 0; k < k_eff; k++)
            printf("%s%d", k ? "," : "", routed[k]);
        printf("]");

        /* mark routed experts, count skip bytes from reader sizes */
        for (int p = 0; p < N_EXPS_PAT; p++) {
            char tname[160];
            snprintf(tname, sizeof(tname), "blk.%d%s", L, EXPS_PAT[p]);
            int ti = find_tensor(&gr, tname);
            if (ti < 0) continue;
            uint64_t tsz = gr.sizes[ti];
            int in_pack = pack_has_tensor(&pi, tname);
            /* stacked tensor: routed fraction = k_eff/n_exp (gate known) */
            if (gate_ok && n_exp > 0) {
                uint64_t per_exp = tsz / (uint64_t)n_exp;
                uint64_t rd = per_exp * (uint64_t)k_eff;
                if (in_pack) { decode_tensors++; decode_bytes += rd; }
                if (tsz > rd) { skip_tensors++; skip_bytes += tsz - rd; }
                else if (!in_pack) { skip_tensors++; skip_bytes += tsz; }
                printf(" %s:%s", EXPS_PAT[p] + 5, in_pack ? "pack" : "MISS");
            } else {
                /* gate unknown: whole stacked blob must decode */
                if (in_pack) { decode_tensors++; decode_bytes += tsz; }
                else { skip_tensors++; skip_bytes += tsz; }
                printf(" %s:%s?", EXPS_PAT[p] + 5, in_pack ? "pack" : "MISS");
            }
        }
        /* geometry home of first routed expert (wtype=GATE) */
        if (gate_ok && k_eff > 0) {
            uint32_t flat = moe_expert_to_flat((uint32_t)L, (uint32_t)routed[0], 0);
            printf(" home_flat=%u", flat);
        }
        printf("\n");
    }

    printf("JOIN: decode=%u tensors (%.1f MB) skip=%u tensors (%.1f MB) gate_miss=%u layers\n",
           decode_tensors, decode_bytes / 1e6, skip_tensors, skip_bytes / 1e6, gate_missing);
    printf("NOTE: static heuristic only — per-token routing needs the callback path (#803).\n");
    return 0;
}
