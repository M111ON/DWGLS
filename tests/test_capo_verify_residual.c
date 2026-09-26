/* test_capo_verify_residual — receipt for the tess_capo_verify path-3 fallback.
 * On current packs every present tensor also has plain capos, so the verify
 * tool's residual branch never fires in practice. This test drives
 * tess_pack_find_residual + tess_pack_apply_residual DIRECTLY on a real pack
 * carrying a residual entry (qwen3 tied weights: output.weight phantom →
 * token_embd.weight, TYPE_CAST Q8_0 → F32) and checks the result against an
 * INDEPENDENT oracle: an inline Q8_0 block dequant written from the GGML spec
 * (x[i] = q[i] * fp16(d)), NOT the implementation under test.
 *
 * Usage: test_capo_verify_residual <model.gguf> <pack.tesspack>
 *   (needs a pack with residual entries, e.g. a tied-weight model pack)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "llama.h"
#include "ggml-backend.h"
#define GGML_TYPE_SIZE_DECL
#include "gguf_reader.h"
#include "geo_tess_container.h"

#define CHECK(c, msg) do { \
    if (!(c)) { printf("FAIL: %s\n", msg); \
        tess_pack_close(&pi); gguf_close(&g); return 1; } \
} while (0)

/* independent oracle: Q8_0 block = fp16 delta + 32×int8 → float. */
static float q80_oracle(const uint8_t *blk, int lane) {
    uint16_t dh;
    memcpy(&dh, blk, 2);
    int8_t q;
    memcpy(&q, blk + 2 + lane, 1);
    return (float)q * fp16_to_float(dh);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <model.gguf> <pack.tesspack>\n", argv[0]);
        return 1;
    }
    GgufReader g;
    if (gguf_open(argv[1], &g) != 0) { printf("FAIL: gguf open\n"); return 1; }
    TESS_PackIndex pi;
    if (tess_pack_open(&pi, argv[2]) != 0) { printf("FAIL: pack open\n"); gguf_close(&g); return 1; }

    printf("residual entries in pack: %u\n", pi.residual_count);
    CHECK(pi.residual_count > 0, "pack carries no residual entries");

    /* the tied-weight phantom: absent from GGUF, present as residual entry */
    int has_out = 0;
    uint32_t ti = 0;
    for (uint32_t i = 0; i < g.n_tensors; i++) {
        if (!strcmp(g.names[i], "token_embd.weight")) { ti = i; }
        if (!strcmp(g.names[i], "output.weight")) has_out = 1;
    }
    printf("output.weight in GGUF: %s (expect absent = tied)\n", has_out ? "YES" : "no");
    CHECK(!has_out, "model not weight-tied; phantom premise broken");

    const TESS_ResidualEntry *re = tess_pack_find_residual(&pi, "output.weight");
    CHECK(re != NULL, "no residual entry for output.weight");
    printf("entry: src=%.*s src_type=%u dst_type=%u transform=%u\n",
           re->src_len, re->src, re->src_type, re->dst_type, re->transform);
    CHECK(re->transform == TESS_TRANSFORM_TYPE_CAST, "expected TYPE_CAST entry");
    CHECK(re->src_len == 18 && !memcmp(re->src, "token_embd.weight", 18), "expected token_embd source");

    /* element count from the SOURCE tensor's GGUF bytes (Q8_0: 34 B / 32 elem) */
    uint64_t src_sz = g.sizes[ti];
    CHECK(src_sz % 34 == 0, "src bytes not a whole Q8_0 block count");
    uint32_t n_elems = (uint32_t)(src_sz / 34 * 32);
    printf("src bytes: %llu → n_elems: %u\n", (unsigned long long)src_sz, n_elems);

    uint8_t *buf = (uint8_t *)malloc((size_t)n_elems * 4);
    CHECK(buf != NULL, "oom");
    int wrote = tess_pack_apply_residual(&pi, re, n_elems, buf);
    printf("applied bytes: %d (expect %u)\n", wrote, n_elems * 4);
    CHECK(wrote == (int)(n_elems * 4), "apply short write");

    /* oracle: dequantize the GGUF source bytes independently, compare ALL */
    const uint8_t *src = g.base + g.data_offset + g.offsets[ti];
    const float *got = (const float *)buf;
    uint32_t bad = 0, checked = 0;
    uint32_t n_blocks = n_elems / 32;
    for (uint32_t b = 0; b < n_blocks; b++) {
        for (int lane = 0; lane < 32; lane++) {
            float exp = q80_oracle(src + (uint64_t)b * 34, lane);
            float v = got[(uint64_t)b * 32 + lane];
            if (v != exp) {
                if (bad < 5)
                    printf("MISMATCH elem %u: got %f expect %f\n",
                           b * 32 + lane, v, exp);
                bad++;
            }
            checked++;
        }
    }
    printf("oracle compare: %u elems, %u bad\n", checked, bad);
    CHECK(bad == 0, "residual-apply bytes differ from independent dequant");

    printf("residual-apply == spec dequant: ALL PASS\n");
    free(buf);
    tess_pack_close(&pi);
    gguf_close(&g);
    return 0;
}
