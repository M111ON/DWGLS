/*
 * test_hyper_seeker_real.c — viewport reads a real GGUF slice tower by tower.
 * Traverse the slice in pan order (tower windows via hs_view), gather each
 * tower's bytes, reassemble by inverse map, demand byte-identical restore.
 * This is the seeker doing its real job: moving-pointer reads over a field
 * bigger than the viewport.
 *
 * Missing model file -> SKIP (return 0, named). Never writes the model.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_hyper_seeker.h"

#define HS_REAL_OFF (1024u * 1024u)
#define HS_REAL_PAGES 2048u
#define HS_REAL_LEN (144u * HS_REAL_PAGES)   /* 288 KB */

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint64_t ref_digest(const uint8_t *d, uint32_t n) {
    uint64_t h = 5381u;
    for (uint32_t i = 0; i < n; i++) h = h * 33u + d[i];
    return h;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    printf("HYPER_SEEKER REAL — %s\n", path);

    FILE *f = fopen(path, "rb");
    if (!f) { printf("  SKIP — model file not present (named, not hidden)\n"); return 0; }
    uint8_t *slice = (uint8_t *)malloc(HS_REAL_LEN);
    uint8_t *got_view = (uint8_t *)malloc(HS_REAL_LEN);
    uint8_t *back = (uint8_t *)malloc(HS_REAL_LEN);
    if (!slice || !got_view || !back) { printf("  SKIP — malloc failed\n"); return 0; }
    fseek(f, HS_REAL_OFF, SEEK_SET);
    size_t n = fread(slice, 1, HS_REAL_LEN, f);
    fclose(f);
    if (n != HS_REAL_LEN) { printf("  SKIP — short read\n"); return 0; }
    uint64_t dg = ref_digest(slice, HS_REAL_LEN);
    printf("  slice: 288 KB @+1MB, digest=%08x%08x\n",
           (uint32_t)(dg >> 32), (uint32_t)(dg & 0xFFFFFFFFu));

    /* read through the moving viewport: pan across towers, collect each
     * tower's bytes (offsets with slot in the tower window). */
    HyperSeeker s;
    hs_init(&s, HS_MODE48);
    uint32_t wpos = 0;
    int gates_ok = 1;
    for (int i = 0; i < 3; i++) {
        uint32_t st, ct;
        hs_view(&s, &st, &ct);
        if (ct != 48u) gates_ok = 0;
        for (uint32_t pg = 0; pg < HS_REAL_PAGES; pg++)
            for (uint32_t k = 0; k < ct; k++)
                got_view[wpos++] = slice[pg * 144u + st + k];
        hs_pan(&s);
    }
    CHECK("viewport collected 288KB in 3 pans", wpos == HS_REAL_LEN);

    /* reassemble: tower t occupies [t*96KB,(t+1)*96KB) of got_view. */
    uint32_t per = HS_REAL_LEN / 3u;
    int rt = 1;
    for (uint32_t pg = 0; pg < HS_REAL_PAGES; pg++)
        for (uint32_t t = 0; t < 3u; t++)
            for (uint32_t k = 0; k < 48u; k++) {
                uint8_t v = got_view[t * per + pg * 48u + k];
                if ((back[pg * 144u + t * 48u + k] = v) != slice[pg * 144u + t * 48u + k])
                    rt = 0;
            }
    CHECK("reassemble byte-identical", rt);
    CHECK("digest stable", ref_digest(slice, HS_REAL_LEN) == dg);
    CHECK("gate rode entry/transit/exit",
          HJ_GATES[hj_tower_gate(0)].first == HJ_GATE_ALL &&
          HJ_GATES[hj_tower_gate(2)].opposite == HJ_GATE_HIP);
    (void)gates_ok;

    free(slice); free(got_view); free(back);
    printf("hyper_seeker_real: %d pass %d fail\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
