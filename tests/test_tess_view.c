/* test_tess_view.c — capo (storage) vs tess (view) split proofs. */
#include <stdio.h>
#include <stdint.h>
#include "../core/geo_tesseract_addr.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    static uint8_t capo[20736];
    for (uint32_t i = 0; i < 20736u; i++) capo[i] = (uint8_t)((i * 37u) & 0xFF);

    /* T1: view reads == flat reads for every (tess,cell,slot). */
    for (uint32_t t = 0; t < 18u; t++)
        for (uint32_t c = 0; c < 8u; c++)
            for (uint32_t s = 0; s < 144u; s++) {
                const uint8_t *p = tess_capo_at(capo, 1, t, c, s);
                uint32_t f = t * 1152u + c * 144u + s;   /* oracle: spec layout */
                if (!p || *p != capo[f]) {
                    printf("FAIL: view @(%u,%u,%u)\n", t, c, s); fails++; goto t2;
                }
            }
t2:
    /* T2: view row == window row (tess*8+cell), all 20736. */
    for (uint32_t f = 0; f < 20736u; f++) {
        uint32_t t, c, s;
        tess_unflat(f, &t, &c, &s);
        if (tess_view_row(t, c) != tess_win_row(f)) {
            printf("FAIL: row @%u\n", f); fails++; break;
        }
    }
    /* T3: OOB is fail-loud. */
    CHECK(tess_capo_at(capo, 1, 18, 0, 0) == NULL, "tess OOB");
    CHECK(tess_capo_at(capo, 1, 0, 8, 0) == NULL, "cell OOB");
    CHECK(tess_capo_at(capo, 1, 0, 0, 144) == NULL, "slot OOB");
    CHECK(tess_capo_at(NULL, 1, 0, 0, 0) == NULL, "null capo");
    CHECK(tess_capo_at(capo, 0, 0, 0, 0) == NULL, "zero cell");
    /* T4: mut write through view lands at flat. */
    *tess_capo_at_mut(capo, 1, 17, 7, 143) = 0xAB;
    CHECK(capo[20735] == 0xAB, "mut view write");

    if (!fails) printf("tess_view: ALL PASS\n");
    return fails ? 1 : 0;
}
