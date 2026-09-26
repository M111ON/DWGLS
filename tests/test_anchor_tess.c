/* test_anchor_tess.c — anchor-routed selective tile serve.
 * Oracle: self-retrieval (each tensor's own centroid must route top-1 to
 * itself) + routed tile-loads memcmp-identical to full-capo loads.
 * Needs model + pack; SKIP (named) when absent.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/anchor_tess.h"
#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define AT_K 24

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *pack  = (argc > 2) ? argv[2] : "I:/model/qwen2.5-0.5b-instruct-q8_0.tesspack";
    printf("ANCHOR_TESS — %s\n", model);

    GgufReader g;
    memset(&g, 0, sizeof(g));
    if (gguf_open(model, &g) != 0) {
        printf("  SKIP — model file not present (named, not hidden)\n");
        return 0;
    }
    TESS_PackIndex pi;
    memset(&pi, 0, sizeof(pi));
    if (tess_pack_open(&pi, pack) != 0) {
        printf("  SKIP — pack file not present (named, not hidden)\n");
        gguf_close(&g);
        return 0;
    }

    uint32_t n = g.n_tensors;
    printf("  tensors: %u\n", n);
    float *X = (float *)calloc((size_t)n * AT_DIM, sizeof(float));
    float *C = (float *)calloc((size_t)AT_K * AT_DIM, sizeof(float));
    CHECK(X && C, "alloc");

    /* index: centroid per tensor over source bytes. */
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = g.data_offset + g.offsets[i];
        at_centroid(g.base + off, g.sizes[i], X + (size_t)i * AT_DIM);
    }
    CHECK(anch_train(X, (int)n, (int)AT_DIM, AT_K, C, NULL) == 0, "train");

    /* T1: routing — exact queries top-1 to own bucket (the serve path);
     * noise sweep must discriminate (space is structured, not collapsed). */
    {
        int top[4], routed = 0, stable5 = 0, stable50 = 0;
        float q[AT_DIM];
        for (uint32_t i = 0; i < n; i++) {
            const float *c0 = X + (size_t)i * AT_DIM;
            int own = anch_assign(c0, C, AT_K, (int)AT_DIM);
            memcpy(q, c0, sizeof(q));
            q[0] += 5.0f;
            if (anch_assign(q, C, AT_K, (int)AT_DIM) == own) stable5++;
            q[0] += 45.0f;
            if (anch_assign(q, C, AT_K, (int)AT_DIM) == own) stable50++;
            if (anch_route(c0, C, AT_K, (int)AT_DIM, 4, top) > 0 && top[0] == own)
                routed++;
        }
        printf("  stable@5: %d/%u stable@50: %d/%u routed-top1: %d/%u\n",
               stable5, n, stable50, n, routed, n);
        CHECK(routed == (int)n, "top-1 routes to own bucket");
        CHECK(stable50 < stable5, "noise discriminates (not collapsed)");
    }

    /* T2: routed selective tile-load == full load (3 probe tensors). */
    {
        uint32_t probes[3] = {0, n / 2, n - 1};
        for (int p = 0; p < 3; p++) {
            uint32_t i = probes[p];
            uint32_t csz = at_cell_size(g.dtypes[i]);
            if (csz == 0) csz = 1;
            uint32_t total = g.sizes[i] / csz;
            uint32_t ncap = (total + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;
            uint8_t *full = (uint8_t *)calloc(1, g.sizes[i]);
            uint8_t *tile = (uint8_t *)calloc(1, g.sizes[i]);
            CHECK(full && tile, "probe alloc");
            /* full capo loads */
            for (uint32_t c = 0; c < ncap; c++) {
                TESS_CapoReader cr;
                CHECK(tess_pack_get_capo(&pi, &cr, g.names[i], c) == 0, "get capo");
                uint32_t cells = (c < ncap - 1) ? TESS_TOTAL_SLOTS : (total - c * TESS_TOTAL_SLOTS);
                uint32_t by = tess_capo_load_range(&cr, 0, cells,
                    full + (uint64_t)c * TESS_TOTAL_SLOTS * csz);
                CHECK(by > 0, "full load");
            }
            /* tile loads (1152-slot units — the view path) */
            for (uint32_t c = 0; c < ncap; c++) {
                TESS_CapoReader cr;
                CHECK(tess_pack_get_capo(&pi, &cr, g.names[i], c) == 0, "get capo t");
                uint32_t cells = (c < ncap - 1) ? TESS_TOTAL_SLOTS : (total - c * TESS_TOTAL_SLOTS);
                for (uint32_t s = 0; s < cells; s += 1152u) {
                    uint32_t k = (cells - s > 1152u) ? 1152u : (cells - s);
                    uint32_t by = tess_capo_load_range(&cr, s, k,
                        tile + ((uint64_t)c * TESS_TOTAL_SLOTS + s) * csz);
                    CHECK(by > 0, "tile load");
                }
            }
            CHECK(memcmp(full, tile, g.sizes[i]) == 0, "tile==full");
            free(full);
            free(tile);
        }
        printf("  selective tile-load: 3/3 identical\n");
    }

    /* T3: index save/load roundtrip routes identically. */
    {
        const char *ip = "build/anchor_tess.idx";
        CHECK(anch_save(ip, C, AT_K, (int)AT_DIM, (int)n) == 0, "save");
        float *C2 = (float *)calloc((size_t)AT_K * AT_DIM, sizeof(float));
        int kd = 0, kn = 0;
        CHECK(anch_load(ip, C2, AT_K, (int)AT_DIM, &kd, &kn) == AT_K, "load");
        CHECK(memcmp(C, C2, (size_t)AT_K * AT_DIM * sizeof(float)) == 0, "index identical");
        free(C2);
        remove(ip);
    }

    free(X);
    free(C);
    tess_pack_close(&pi);
    gguf_close(&g);
    if (!fails) printf("anchor_tess: ALL PASS\n");
    return fails ? 1 : 0;
}
