/* test_moe_jet.c — jet want-stream over a REAL MoE routing trace.
 *
 * No region file needed: routing comes from huihui-1b's actual router-gate
 * tensors (col-sum score + top-K, same math as moe_expert_route.c); want
 * addresses come from moe_expert_to_flat (real geometry). The dispatch stays
 * simulated — this proves the trace HAS jet-able structure (locality mix,
 * coalesce ratio), not that I/O was batched.
 * Oracle: gate bytes from the GGUF + flat-address math. SKIP if absent.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_reader.h"
#include "../core/moe_expert_addr.h"
#include "../core/infra/geo_gpu_pipeline.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define MJ_TOPK 4
#define MJ_WTYPE 3

static uint16_t f16b(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static float f16f(uint16_t h) {
    int s = (h >> 15) & 1, e = ((h >> 10) & 0x1f) - 15, m = h & 0x3ff;
    if (e == -15 && m == 0) return s ? -0.0f : 0.0f;
    float v = (e == -15) ? (float)m / 1024.0f
                         : (1.0f + (float)m / 1024.0f) * (float)(1 << (e > 0 ? e : 0));
    if (e < 0 && e != -15) v /= (float)(1 << (-e));
    return s ? -v : v;
}

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "F:/model/huihui-moe-1b-q4_k_m.gguf";
    printf("MOE_JET — %s\n", model);

    GgufReader g;
    memset(&g, 0, sizeof(g));
    if (gguf_open(model, &g) != 0) {
        printf("  SKIP — model not present (named, not hidden)\n");
        return 0;
    }

    /* find max layer from expert tensor names */
    int max_layer = -1;
    for (uint32_t i = 0; i < g.n_tensors; i++) {
        const char *p = strstr(g.names[i], "blk.");
        if (!p) continue;
        if (strstr(g.names[i], "_exps.weight")) {
            int l = atoi(p + 4);
            if (l > max_layer) max_layer = l;
        }
    }
    CHECK(max_layer > 0, "moe layers found");
    int n_layers = max_layer + 1;
    printf("  layers: %d\n", n_layers);

    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);

    uint32_t strat_hist[3] = {0, 0, 0};   /* C1, C3, B — check jet_select.h ids */
    uint32_t prev_flat = 0;
    int have_prev = 0;
    uint32_t tick = 0, wants = 0;

    for (int layer = 0; layer < n_layers; layer++) {
        char gate_name[128];
        snprintf(gate_name, sizeof(gate_name), "blk.%d.ffn_gate_inp.weight", layer);
        int gi = -1;
        for (uint32_t i = 0; i < g.n_tensors; i++)
            if (strcmp(g.names[i], gate_name) == 0) { gi = (int)i; break; }
        int topk[MJ_TOPK] = {0, 1, 2, 3};
        int n_exp = 0;
        if (gi >= 0) {
            uint64_t off = g.data_offset + g.offsets[gi];
            int n_embd = (int)g.dims[(size_t)gi * 4 + 0];
            n_exp = (int)g.dims[(size_t)gi * 4 + 1];
            if (n_embd > 0 && n_exp > 0 && g.sizes[gi] == (uint32_t)(n_embd * n_exp * 2)) {
                /* col-sum score (same math as moe_expert_route.c) */
                float best[MJ_TOPK];
                for (int k = 0; k < MJ_TOPK; k++) { best[k] = -1e30f; topk[k] = -1; }
                for (int e = 0; e < n_exp; e++) {
                    double sum = 0;
                    for (int j = 0; j < n_embd; j++)
                        sum += f16f(f16b(g.base + off + (size_t)(j * n_exp + e) * 2));
                    float s = (float)sum;
                    for (int k = 0; k < MJ_TOPK; k++)
                        if (s > best[k]) {
                            for (int m = MJ_TOPK - 1; m > k; m--) {
                                best[m] = best[m - 1]; topk[m] = topk[m - 1];
                            }
                            best[k] = s; topk[k] = e;
                            break;
                        }
                }
            }
        }
        /* wants: top-K experts x 3 wtypes at real flat addresses */
        for (int k = 0; k < MJ_TOPK; k++) {
            if (topk[k] < 0) topk[k] = k;
            for (int w = 0; w < MJ_WTYPE; w++) {
                uint32_t flat = moe_expert_to_flat((uint32_t)layer, (uint32_t)topk[k], (uint32_t)w);
                uint32_t L = 0;
                if (have_prev) {
                    uint32_t hop = prev_flat > flat ? prev_flat - flat : flat - prev_flat;
                    L = hop >> 6;               /* 64-flat blocks; same-layer hops stay small */
                    if (L > 11) L = 11;
                }
                int s = geo_pipeline_want(&ctx, tick++, L);
                if (s >= 0 && s < 3) strat_hist[s]++;
                prev_flat = flat;
                have_prev = 1;
                wants++;
            }
        }
        geo_pipeline_tick_n(&ctx, 12);   /* one spine round per layer */
    }
    /* drain */
    for (int i = 0; i < 24 && ctx.wants_ready; i++) geo_pipeline_tick(&ctx);

    printf("  wants=%u coalesced=%u bridges=%u\n", wants, ctx.wants_coalesced, ctx.bridges);
    printf("  strat: C3=%u B=%u C1=%u\n", strat_hist[0], strat_hist[1], strat_hist[2]);

    CHECK(wants == (uint32_t)(n_layers * MJ_TOPK * MJ_WTYPE), "want count exact");
    CHECK(ctx.wants_ready == 0, "all wants drained");
    CHECK(ctx.wants_coalesced == wants, "coalesce ratio 1.0 (nothing lost)");
    CHECK(ctx.bridges >= 1, "bridges fired");
    CHECK(strat_hist[0] + strat_hist[1] + strat_hist[2] == wants, "histogram sums");
    CHECK(strat_hist[2] > 0, "C1 locality exists (same-layer hops)");

    gguf_close(&g);
    if (!fails) printf("moe_jet: ALL PASS\n");
    return fails ? 1 : 0;
}
