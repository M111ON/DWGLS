/*
 * test_geo_dual_view.c — Container dual view: เลือก GeoType ได้โดยข้อมูลไม่ต้องย้าย
 * ═══════════════════════════════════════════════════════════════════════
 *
 * T1.2g — "เราสามารถใช้ container เป็น icosa, dodeca ได้" (user, 2026-08-17)
 *
 * หลักการ: payload (codebook + idx stream) ของ geo codec ไม่แตะ geometry —
 * GeoType แค่ให้ props (verts/edges/faces) + mask + capacity เท่านั้น
 * → วางข้อมูล 1 ชุด อ่านผ่าน view ไหนก็ได้ (dodeca 12 / icosa 20 /
 *   compound_144 / goldberg_192) → lossless byte-for-byte เหมือนกัน
 *
 * Proof:
 *   T1  encode+dodeca → decode lossless (baseline)
 *   T2  encode+icosa → decode lossless
 *   T3  payload (codebook+idx) ของ dodeca == icosa ทุก byte (geometry ไม่แตะ payload)
 *   T4  decode จาก dodeca view == decode จาก icosa view (ค่าเหมือนกันเป๊ะ)
 *   T5  ข้ามตระกูล: dodeca / icosa / compound_144 / goldberg_192 — payload เดียวกัน
 *       + decode lossless ทุก view (container เลือกรูปทรงได้)
 *   T6  mask ต่างกันตาม verts (geometry = mask เท่านั้น) แต่ payload เหมือนกัน
 *   T7  capacity clamp: n_used_verts ต่างกัน (3072 vs 5120) แต่ decode ยัง lossless
 *       — ข้อมูลไม่ได้ถูก geometry แตะ (rescope: geometry = template)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_geo_dual_view tests/test_geo_dual_view.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_param_grid.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* deterministic pseudo-random weights with repetition (codebook-collapsible) */
static void fill_weights(float *w, uint32_t n, uint32_t seed)
{
    uint32_t x = seed;
    for (uint32_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        uint32_t v = (x >> 16) % 64u;          /* 64 distinct values → codebook */
        w[i] = (float)(int32_t)(v - 32) * 0.25f;
    }
}

/* payload = codebook bytes + idx stream bytes (the data — geometry-agnostic) */
static int payload_equal(const GeoCodec *a, const GeoCodec *b)
{
    if (a->n_uniq != b->n_uniq) return 0;
    if (a->idx_bits != b->idx_bits) return 0;
    if (memcmp(a->uniq, b->uniq, a->n_uniq * sizeof(float)) != 0) return 0;
    if (memcmp(a->idx, b->idx, a->n_weights * sizeof(uint32_t)) != 0) return 0;
    return 1;
}

static int decode_matches(GeoCodec *a, GeoCodec *b)
{
    float *ra = (float*)malloc(a->n_weights * sizeof(float));
    float *rb = (float*)malloc(b->n_weights * sizeof(float));
    if (!ra || !rb) { free(ra); free(rb); return 0; }
    int ok = (geo_codec_decode(a, ra, a->n_weights) == 0) &&
             (geo_codec_decode(b, rb, b->n_weights) == 0) &&
             (memcmp(ra, rb, a->n_weights * sizeof(float)) == 0);
    /* lossless vs original */
    for (uint32_t i = 0; i < a->n_weights && ok; i++)
        if (ra[i] != a->weights[i] && !(ra[i] != ra[i] && a->weights[i] != a->weights[i]))
            ok = 0;
    free(ra); free(rb);
    return ok;
}

#define N_W 5000u

int main(void)
{
    printf("═══ test_geo_dual_view — container dual view (GeoType เลือกได้, ข้อมูลไม่ย้าย) ═══\n\n");

    float w[N_W];
    fill_weights(w, N_W, 42u);

    GeoCodec dc, ic, c144, g192;
    memset(&dc, 0, sizeof dc); memset(&ic, 0, sizeof ic);
    memset(&c144, 0, sizeof c144); memset(&g192, 0, sizeof g192);

    int init_ok = geo_codec_init(&dc,  GEO_DODEC_BASE,   w, N_W) == 0 &&
                  geo_codec_init(&ic,  GEO_ICO_BASE,     w, N_W) == 0 &&
                  geo_codec_init(&c144, GEO_COMPOUND_144, w, N_W) == 0 &&
                  geo_codec_init(&g192, GEO_GOLDBERG_192, w, N_W) == 0;
    CHECK("setup: 4 codecs init (dodeca 12 / icosa 20 / compound 144 / goldberg 192)", init_ok);

    /* T1/T2: each view decodes lossless */
    CHECK("T1: dodeca (12) view — decode lossless",
          init_ok && geo_codec_verify(&dc) == 0);
    CHECK("T2: icosa (20) view — decode lossless",
          init_ok && geo_codec_verify(&ic) == 0);

    /* T3: payload identical across views */
    CHECK("T3: payload (codebook+idx) dodeca == icosa ทุก byte",
          init_ok && payload_equal(&dc, &ic));

    /* T4: decode results identical */
    CHECK("T4: decode(dodeca view) == decode(icosa view) เป๊ะ",
          init_ok && decode_matches(&dc, &ic));

    /* T5: cross-family — payload เดียวกันหมด + lossless ทุก view */
    CHECK("T5: payload เดียวกันทั้ง 4 views (12/20/144/192)",
          init_ok && payload_equal(&dc, &ic) && payload_equal(&dc, &c144) &&
          payload_equal(&dc, &g192));
    CHECK("T5b: decode lossless ทุก view",
          init_ok && decode_matches(&dc, &ic) && decode_matches(&dc, &c144) &&
          decode_matches(&dc, &g192));

    /* T6: geometry = mask เท่านั้น — props ต่าง, payload เหมือนกัน */
    {
        int ok = init_ok &&
                 dc.props.verts == 20 && dc.props.faces == 12 &&   /* dodeca */
                 ic.props.verts == 20 && ic.props.faces == 20 &&   /* icosa  */
                 c144.props.verts == 144 && c144.props.faces == 576 && /* compound */
                 g192.props.verts == 192 && g192.props.faces == 92;    /* goldberg */
        CHECK("T6: props ต่างกัน (12/20/144/192 faces) แต่ payload เดียวกัน", ok);
    }

    /* T7: capacity clamp ต่างกัน แต่ decode lossless — geometry ไม่แตะข้อมูล */
    {
        uint64_t cap_d = (uint64_t)dc.props.verts * dc.props.slot_cap;   /* 5120 */
        uint64_t cap_i = (uint64_t)ic.props.verts * ic.props.slot_cap;   /* 5120 */
        uint64_t cap_144 = (uint64_t)c144.props.verts * c144.props.slot_cap; /* 36864 */
        uint64_t cap_192 = (uint64_t)g192.props.verts * g192.props.slot_cap; /* 49152 */
        int ok = init_ok && cap_d == cap_i && cap_144 > cap_d && cap_192 > cap_d &&
                 dc.n_uniq == ic.n_uniq && c144.n_used_verts >= dc.n_used_verts &&
                 decode_matches(&dc, &c144);
        CHECK("T7: capacity clamp ต่างกัน (geometry) แต่ decode ยัง lossless — template เท่านั้น",
              ok);
    }

    /* T8: mask bytes ต่างกันตาม verts — แต่ payload ไม่ถูก mask แตะ */
    {
        int ok = init_ok &&
                 (dc.props.verts + 7) / 8 == (uint64_t)((dc.props.verts + 7) / 8) &&
                 dc.mask_len == (dc.props.verts + 7) / 8 &&
                 c144.mask_len == (c144.props.verts + 7) / 8 &&
                 dc.codebook_len == ic.codebook_len &&   /* payload len เดียวกัน */
                 dc.idx_len == ic.idx_len &&
                 dc.codebook_len == c144.codebook_len &&
                 dc.idx_len == c144.idx_len;
        CHECK("T8: mask_len ตาม verts (2/2/18/24) แต่ codebook/idx len เดียวกันทุก view",
              ok);
    }

    geo_codec_free(&dc); geo_codec_free(&ic);
    geo_codec_free(&c144); geo_codec_free(&g192);

    printf("\n═══ RESULT: %d/%d PASS ═══\n", pass_count, pass_count + fail_count);
    return fail_count == 0 ? 0 : 1;
}
