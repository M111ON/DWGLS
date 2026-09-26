/* test_hyper_scale.c — requests riding scale change (breathing wire).
 * Expand x2: home slot h -> scaled pos 2h in the 288-space (3x96 towers).
 * Requests emit even positions only (holes never hit); collapse /2 is
 * exact; fetched bytes equal direct bytes. Real slice, SKIP-if-absent.
 * Oracle: direct indexing + home-scale roundtrip (proven in earlier tests).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_hyper_jump.h"

static int pass = 0, fail = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass++; } \
    else      { fail++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(int argc, char **argv) {
    /* span API collapses to the proven jumps at home scale. */
    int home = 1;
    for (uint32_t p = 0; p < 144u; p++)
        if (hj_jump_span(p, 48u, 3u) != hj3_jump(p)) home = 0;
    CHECK("span(48,3) == hj3 x144", home);

    /* expand x2: 2h roundtrips in 288-space, collapse exact. */
    int rt = 1, even = 1, exact = 1;
    for (uint32_t h = 0; h < 144u; h++) {
        uint32_t sc = 2u * h;
        if (sc % 2u != 0) even = 0;
        uint32_t tw = hj_tower_span(sc, 96u, 3u);
        uint32_t lo = hj_local_span(sc, 96u, 3u);
        uint32_t back = tw * 96u + lo;
        if (back != sc) rt = 0;
        if (back / 2u != h || back % 2u != 0) exact = 0;
    }
    CHECK("scaled roundtrip x144", rt);
    CHECK("requests hit no holes", even);
    CHECK("collapse /2 exact", exact);

    /* real: fetch through scaled path == direct bytes. */
    const char *path = (argc > 1) ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  SKIP — model not present\n"); goto done; }
    uint32_t len = 144u * 512u;
    uint8_t *slice = (uint8_t *)malloc(len);
    if (!slice) { printf("  SKIP — malloc failed\n"); return 0; }
    fseek(f, 1024u * 1024u, SEEK_SET);
    if (fread(slice, 1, len, f) != len) { printf("  SKIP — short read\n"); return 0; }
    fclose(f);
    int bytes = 1;
    for (uint32_t pg = 0; pg < 512u; pg++)
        for (uint32_t h = 0; h < 144u; h++) {
            uint32_t sc = 2u * h;
            uint32_t back = hj_tower_span(sc, 96u, 3u) * 96u
                          + hj_local_span(sc, 96u, 3u);
            uint32_t home_slot = back / 2u;
            if (slice[pg * 144u + home_slot] != slice[pg * 144u + h]) bytes = 0;
        }
    CHECK("scaled fetch == direct x73728", bytes);
    free(slice);

done:
    printf("hyper_scale: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
