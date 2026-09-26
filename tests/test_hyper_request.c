/* test_hyper_request.c — request pipeline: coarse -> fine -> fetch -> output.
 * Input: sparse requested slots (every 17th byte, like real retrieval).
 * Each request drills GJ-coarse -> HJ-fine, fetches the byte, emits output.
 * Oracle: direct-index gather (independent path). Process step: XOR-fold
 * receipt over output. Real GGUF slice, SKIP-if-absent.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_hyper_resolve.h"

static int pass = 0, fail = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass++; } \
    else      { fail++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  SKIP — model not present\n"); return 0; }
    uint32_t len = 144u * 2048u;
    uint8_t *slice = (uint8_t *)malloc(len);
    if (!slice) { printf("  SKIP — malloc failed\n"); return 0; }
    fseek(f, 1024u * 1024u, SEEK_SET);
    if (fread(slice, 1, len, f) != len) { printf("  SKIP — short read\n"); return 0; }
    fclose(f);

    /* sparse requests: every 17th byte across the slice. */
    uint32_t nreq = len / 17u;
    uint8_t *out = (uint8_t *)malloc(nreq);
    uint8_t *expect = (uint8_t *)malloc(nreq);
    int ok = 1;
    uint8_t fold = 0;
    int towers[3] = {0, 0, 0};
    for (uint32_t r = 0; r < nreq; r++) {
        uint32_t want = r * 17u;              /* requested byte offset */
        uint32_t slot = want % 144u;          /* slot in minimap */
        uint32_t page = want / 144u;
        HjCell c = hjr_resolve(slot, HS_MODE48);   /* coarse -> fine */
        uint32_t rt = hjr_point(c, HS_MODE48);     /* point back */
        if (rt != slot) ok = 0;
        out[r] = slice[page * 144u + rt];     /* fetch through resolve */
        expect[r] = slice[want];              /* oracle: direct index */
        fold ^= out[r];
        towers[c.tower] = 1;
    }
    CHECK("resolve/fetch matches direct index", ok && memcmp(out, expect, nreq) == 0);
    CHECK("requests rode all 3 towers", towers[0] && towers[1] && towers[2]);

    /* process step: XOR-fold receipt recomputed independently. */
    uint8_t ref = 0;
    for (uint32_t r = 0; r < nreq; r++) ref ^= slice[r * 17u];
    CHECK("output fold matches", fold == ref);
    printf("  requests=%u fold=%02x\n", nreq, fold);

    free(slice); free(out); free(expect);
    printf("hyper_request: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
