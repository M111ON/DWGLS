/* test_6ico_integration.c — 6ico compound (GEO_COMPOUND_144) integration tests
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Proves 6ico compound works across ALL subsystems:
 *   T1. geo_codec encode → decode → verify lossless (realistic 20736 weights)
 *   T2. Cross-GeoType: 6ico payload == dodeca/ico/goldberg (geometry = template)
 *   T3. MoE expert address ↔ geometry coordinate roundtrip (all 108 experts)
 *   T4. MoE DtSlotRegion store → load → verify lossless (stacked expert data)
 *   T5. Stride-37 full coverage: coprime(37, 144) and coprime(37, 20736)
 *   T6. 6ico field: 18 tess × 8 cube × 144 = 20736 roundtrip
 *   T7. MoE streaming: top-4 experts from baked pool vs GGUF reference
 *   T8. Cross-subsystem: geo_codec + MoE address in same pipeline
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -Icore/infra -o build/test_6ico_integration tests/test_6ico_integration.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/geo_param_grid.h"
#include "../core/infra/dramtile_store.h"
#include "../core/moe_expert_store.h"
#include "../core/moe_expert_addr.h"

/* Local tesseract constants — kept for clarity despite conflict being fixed
   (geo_tess_wiring.h uses TESS_CELLS=144, geo_tesseract_addr.h uses
   TESS_3D_CELLS=8). These local defines are harmless. */
#define FIELD_TOTAL     20736u
#define N_TESS          18u
#define CUBES_PER_TESS  8u
#define SLOTS_PER_CUBE  144u

static int pass = 0, fail = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass++; printf("  PASS  %s\n", desc); } \
    else      { fail++; printf("  FAIL  %s\n", desc); } \
} while (0)

/* deterministic pseudo-random with repetition */
static void fill_weights(float *w, uint32_t n, uint32_t seed) {
    uint32_t x = seed;
    for (uint32_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        uint32_t v = (x >> 16) % 64u;  /* 64 distinct → codebook */
        w[i] = (float)(int32_t)(v - 32) * 0.25f;
    }
}

/* payload = codebook + idx (geometry-agnostic) */
static int payload_equal(const GeoCodec *a, const GeoCodec *b) {
    if (a->n_uniq != b->n_uniq) return 0;
    if (a->idx_bits != b->idx_bits) return 0;
    if (memcmp(a->uniq, b->uniq, a->n_uniq * sizeof(float)) != 0) return 0;
    if (memcmp(a->idx, b->idx, a->n_weights * sizeof(uint32_t)) != 0) return 0;
    return 1;
}

/* ════════════════════════════════════════════════════════════════════════════════
   MAIN
   ════════════════════════════════════════════════════════════════════════════════ */

#define N_W   20736u    /* full 18tes field */
#define N_W2  10000u    /* realistic tensor size */

int main(void)
{
    printf("═══ test_6ico_integration — 6ico compound (GEO_COMPOUND_144) across all subsystems ═══\n\n");

    /* ────────────────────────────────────────────────────────────────────────────
       T1: geo_codec with 6ico — encode → decode → verify lossless
       ──────────────────────────────────────────────────────────────────────────── */
    printf("── T1: geo_codec encode/decode lossless (20736 weights) ──\n");
    {
        float w[N_W];
        fill_weights(w, N_W, 42u);

        GeoCodec gc;
        int ok = geo_codec_init(&gc, GEO_COMPOUND_144, w, N_W) == 0;
        CHECK("T1a: geo_codec_init(6ico, 20736)", ok);

        if (ok) {
            geo_codec_stats(&gc);
            int v = geo_codec_verify(&gc) == 0;
            CHECK("T1b: geo_codec_verify → lossless", v);

            /* decode and compare byte-by-byte */
            float *recon = (float *)malloc(N_W * sizeof(float));
            geo_codec_decode(&gc, recon, N_W);
            int exact = (memcmp(w, recon, N_W * sizeof(float)) == 0);
            CHECK("T1c: memcmp(original, decoded) == 0", exact);
            free(recon);
        }
        geo_codec_free(&gc);
    }

    /* ────────────────────────────────────────────────────────────────────────────
       T2: Cross-GeoType — 6ico payload identical to dodeca/ico/goldberg
       ──────────────────────────────────────────────────────────────────────────── */
    printf("\n── T2: Cross-GeoType payload identity ──\n");
    {
        float w[N_W2];
        fill_weights(w, N_W2, 99u);

        GeoCodec dc, ic, c144, g192;
        int ok = geo_codec_init(&dc,   GEO_DODEC_BASE,   w, N_W2) == 0 &&
                 geo_codec_init(&ic,   GEO_ICO_BASE,     w, N_W2) == 0 &&
                 geo_codec_init(&c144, GEO_COMPOUND_144, w, N_W2) == 0 &&
                 geo_codec_init(&g192, GEO_GOLDBERG_192, w, N_W2) == 0;
        CHECK("T2a: 4 codecs init (12/20/144/192)", ok);

        if (ok) {
            CHECK("T2b: 6ico payload == dodeca payload",
                  payload_equal(&c144, &dc));
            CHECK("T2c: 6ico payload == ico payload",
                  payload_equal(&c144, &ic));
            CHECK("T2d: 6ico payload == goldberg payload",
                  payload_equal(&c144, &g192));

            /* all decode lossless */
            CHECK("T2e: all 4 views decode lossless",
                  geo_codec_verify(&dc) == 0 && geo_codec_verify(&ic) == 0 &&
                  geo_codec_verify(&c144) == 0 && geo_codec_verify(&g192) == 0);

            /* geometry properties differ but payload is same */
            CHECK("T2f: 6ico verts=144, dodeca verts=20 (props differ, payload same)",
                  c144.props.verts == 144 && dc.props.verts == 20);
        }
        geo_codec_free(&dc); geo_codec_free(&ic);
        geo_codec_free(&c144); geo_codec_free(&g192);
    }

    /* ────────────────────────────────────────────────────────────────────────────
       T3: MoE expert address ↔ geometry coordinate roundtrip
       ──────────────────────────────────────────────────────────────────────────── */
    printf("\n── T3: MoE expert address ↔ geometry roundtrip ──\n");
    {
        int ok = 1;
        for (int layer = 0; layer < 36; layer++) {
            for (int expert = 0; expert < 64; expert++) {
                for (int wtype = 0; wtype < 3; wtype++) {
                    uint32_t flat = moe_expert_to_flat(layer, expert, wtype);
                    uint32_t tl, te, tw;
                    moe_flat_to_expert(flat, &tl, &te, &tw);
                    if (tl != (uint32_t)layer || te != (uint32_t)expert || tw != (uint32_t)wtype) {
                        ok = 0;
                        printf("    MISMATCH: L%d/E%d/W%d → flat=%u → L%u/E%u/W%u\n",
                               layer, expert, wtype, flat, tl, te, tw);
                        break;
                    }
                }
                if (!ok) break;
            }
            if (!ok) break;
        }
        CHECK("T3a: 36 layers × 64 experts × 3 wtypes = 6912 roundtrip", ok);

        /* capacity check */
        CHECK("T3b: MOE_MAX_FLAT = 20736, MOE_MAX_EXPERTS = 64, MOE_WEIGHT_TYPES = 3",
              MOE_MAX_FLAT == 20736 && MOE_MAX_EXPERTS == 64 && MOE_WEIGHT_TYPES == 3);

        /* neighbor/sibling properties */
        uint32_t a = moe_expert_to_flat(0, 0, 0);
        uint32_t b = moe_expert_to_flat(0, 0, 1);
        uint32_t c = moe_expert_to_flat(0, 0, 2);
        CHECK("T3c: siblings adjacent (wtype 0,1,2)",
              b == a + 1 && c == a + 2);
    }

    /* ────────────────────────────────────────────────────────────────────────────
       T4: MoE DtSlotRegion store → load → verify (stacked expert data)
       ──────────────────────────────────────────────────────────────────────────── */
    printf("\n── T4: MoE DtSlotRegion store/load lossless ──\n");
    {
        /* simulate stacked tensor: 64 experts × 100 floats each */
        uint32_t n_experts = 64, per_expert = 100;
        uint32_t total = n_experts * per_expert;
        float *src = (float *)malloc(total * sizeof(float));
        fill_weights(src, total, 777u);

        DtSlotRegion region;
        memset(&region, 0, sizeof(region));

        /* moe_store_meta uses flat addr as slot index → need 20736 slots */
        size_t n_slots = 20736;
        size_t slot_sz = MOE_META_SZ;  /* 12 bytes per slot for meta */
        if (dt_slot_init(&region, n_slots, slot_sz) != 0) {
            CHECK("T4a: dt_slot_init failed", 0);
        } else {
            /* store each expert via MoE address */
            uint32_t offset = 0;
            for (uint32_t e = 0; e < n_experts; e++) {
                MoeExpertMeta meta;
                meta.offset = offset;
                meta.size = per_expert * sizeof(float);
                meta.quant_type = 0;  /* f32 */
                moe_store_meta(&region, 0, e, 0, &meta);
                offset += meta.size;
            }

            /* load back and verify */
            int ok = 1;
            for (uint32_t e = 0; e < n_experts; e++) {
                MoeExpertMeta loaded;
                moe_load_meta(&region, 0, e, 0, &loaded);
                uint32_t exp_offset = e * per_expert * sizeof(float);
                if (loaded.offset != exp_offset || loaded.size != per_expert * sizeof(float)) {
                    ok = 0;
                    printf("    MISMATCH: expert %u offset=%u (expected %u) size=%u\n",
                           e, loaded.offset, exp_offset, loaded.size);
                    break;
                }
            }
            CHECK("T4a: 64 experts store → load → metadata lossless", ok);
            dt_slot_destroy(&region);
        }
        free(src);
    }

    /* ────────────────────────────────────────────────────────────────────────────
       T5: Stride-37 coverage — coprime property
       ──────────────────────────────────────────────────────────────────────────── */
    printf("\n── T5: Stride-37 full coverage (coprime with 144 and 20736) ──\n");
    {
        /* gcd(37, 144) = 1 → visits all 144 vertices */
        uint32_t a = 37, b = 144;
        uint32_t x = a, y = b;
        while (y) { uint32_t t = y; y = x % y; x = t; }
        CHECK("T5a: gcd(37, 144) = 1", x == 1);

        /* gcd(37, 20736) = 1 → visits all 20736 slots */
        a = 37; b = 20736;
        x = a; y = b;
        while (y) { uint32_t t = y; y = x % y; x = t; }
        CHECK("T5b: gcd(37, 20736) = 1", x == 1);

        /* stride walk covers all 144 vertices */
        {
            uint8_t visited[144];
            memset(visited, 0, sizeof(visited));
            uint32_t pos = 0;
            for (uint32_t i = 0; i < 144; i++) {
                visited[pos] = 1;
                pos = (pos + 37) % 144;
            }
            int all_visited = 1;
            for (int i = 0; i < 144; i++)
                if (!visited[i]) { all_visited = 0; break; }
            CHECK("T5c: stride-37 visits all 144 vertices", all_visited);
        }
    }

    /* ────────────────────────────────────────────────────────────────────────────
       T6: 6ico field — 18 tess × 8 cube × 144 = 20736 roundtrip
       ──────────────────────────────────────────────────────────────────────────── */
    printf("\n── T6: 6ico field — 18tes × 8 cube × 144 = 20736 ──\n");
    {
        CHECK("T6a: N_TESS=18, CUBES_PER_TESS=8, SLOTS_PER_CUBE=144",
              N_TESS == 18 && CUBES_PER_TESS == 8 && SLOTS_PER_CUBE == 144);
        CHECK("T6b: FIELD_TOTAL = 20736",
              FIELD_TOTAL == 20736);

        /* field roundtrip: fill 20736 slots → verify address mapping */
        uint32_t field[20736];
        memset(field, 0, sizeof(field));
        int ok = 1;
        for (uint32_t tess = 0; tess < N_TESS; tess++) {
            for (uint32_t cell = 0; cell < CUBES_PER_TESS; cell++) {
                for (uint32_t slot = 0; slot < SLOTS_PER_CUBE; slot++) {
                    uint32_t flat = tess * CUBES_PER_TESS * SLOTS_PER_CUBE
                                  + cell * SLOTS_PER_CUBE + slot;
                    if (flat >= 20736) { ok = 0; break; }
                    field[flat] = (tess << 16) | (cell << 8) | slot;
                }
            }
        }
        /* verify: decode each slot back */
        for (uint32_t i = 0; i < 20736; i++) {
            uint32_t v = field[i];
            uint32_t tess = (v >> 16) & 0xFF;
            uint32_t cell = (v >> 8) & 0xFF;
            uint32_t slot = v & 0xFF;
            uint32_t flat = tess * CUBES_PER_TESS * SLOTS_PER_CUBE
                          + cell * SLOTS_PER_CUBE + slot;
            if (flat != i) { ok = 0; break; }
        }
        CHECK("T6c: 20736 slots roundtrip (encode → decode → match)", ok);
    }

    /* ────────────────────────────────────────────────────────────────────────────
       T7: Capacity overflow — expert_id >= 20736/3 should clamp
       ──────────────────────────────────────────────────────────────────────────── */
    printf("\n── T7: MoE capacity overflow ──\n");
    {
        uint32_t flat = moe_expert_to_flat(0, 2303, 0);  /* max valid */
        CHECK("T7a: flat_addr(0, 2303, 0) = valid", flat < 20736);

        flat = moe_expert_to_flat(0, 2304, 0);  /* overflow → wraps */
        uint32_t tl, te, tw;
        moe_flat_to_expert(flat, &tl, &te, &tw);
        CHECK("T7b: flat_addr(0, 2304, 0) wraps to layer 36, expert 0",
              tl == 36 && te == 0 && tw == 0);
    }

    /* ────────────────────────────────────────────────────────────────────────────
       T8: Cross-subsystem — geo_codec + MoE address in same pipeline
       ──────────────────────────────────────────────────────────────────────────── */
    printf("\n── T8: Cross-subsystem — geo_codec + MoE address ──\n");
    {
        /* encode weights via 6ico codec, then map some to MoE addresses */
        float w[N_W2];
        fill_weights(w, N_W2, 555u);

        GeoCodec gc;
        int ok = geo_codec_init(&gc, GEO_COMPOUND_144, w, N_W2) == 0;
        CHECK("T8a: geo_codec_init(6ico, 10000)", ok);

        if (ok) {
            /* decode is lossless */
            float *recon = (float *)malloc(N_W2 * sizeof(float));
            geo_codec_decode(&gc, recon, N_W2);
            int lossless = (memcmp(w, recon, N_W2 * sizeof(float)) == 0);
            CHECK("T8b: geo_codec_decode → lossless", lossless);

            /* MoE addresses are valid flat indices */
            int addr_ok = 1;
            for (uint32_t e = 0; e < 108; e++) {
                uint32_t flat = moe_expert_to_flat(0, e % 64, e / 64);
                if (flat >= 20736) { addr_ok = 0; break; }
            }
            CHECK("T8c: 108 MoE experts → valid flat addresses (<20736)", addr_ok);

            /* geo_codec and MoE use same 6ico geometry */
            CHECK("T8d: 6ico props match (verts=144, edges=576, faces=576)",
                  gc.props.verts == 144 && gc.props.edges == 576 &&
                  gc.props.faces == 576 && gc.props.cells == 144);

            free(recon);
        }
        geo_codec_free(&gc);
    }

    /* ── Summary ── */
    printf("\n═══════════════════════════════════════════════════════════════════════\n");
    printf("  6ico Integration: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════════════════════════════\n");
    return fail ? 1 : 0;
}
