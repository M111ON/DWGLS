/* tests/test_mm_wang6.c — 6-direction Wang บน 10^3 maze cube */
#include <stdio.h>
#include <assert.h>
#include "../core/mm_wang6.h"

int main(void) {
    int pass = 0, fail = 0;
#define OK(c) do { if (c) { pass++; } else { fail++; printf("FAIL line %d\n", __LINE__); } } while (0)

    /* T1: opposite pairs */
    OK(mw6_opp(0) == 1 && mw6_opp(1) == 0 && mw6_opp(2) == 3 &&
       mw6_opp(3) == 2 && mw6_opp(4) == 5 && mw6_opp(5) == 4);

    /* T2: ทุก internal face ใน 10^3 เปิด (2700 faces) */
    unsigned open = 0, total = 0;
    for (unsigned x = 0; x < 10; x++)
        for (unsigned y = 0; y < 10; y++)
            for (unsigned z = 0; z < 10; z++) {
                if (x + 1 < 10) { total++; open += (unsigned)mw6_gate(x, y, z, x + 1, y, z); }
                if (y + 1 < 10) { total++; open += (unsigned)mw6_gate(x, y, z, x, y + 1, z); }
                if (z + 1 < 10) { total++; open += (unsigned)mw6_gate(x, y, z, x, y, z + 1); }
            }
    printf("internal faces: %u/%u open\n", open, total);
    OK(total == 2700 && open == total);

    /* T3: สีกระจายครบ 8 (ไม่เอียงข้างเดียว) */
    unsigned hist[8] = {0};
    for (unsigned x = 0; x < 10; x++)
        for (unsigned y = 0; y < 10; y++)
            for (unsigned z = 0; z < 10; z++)
                for (unsigned d = 0; d < 6; d++) {
                    uint8_t c = mw6_cell_color(x, y, z, (uint8_t)d);
                    if (c < 8) hist[c]++;
                }
    unsigned mn = hist[0], mx = hist[0];
    for (int i = 1; i < 8; i++) {
        if (hist[i] < mn) mn = hist[i];
        if (hist[i] > mx) mx = hist[i];
    }
    printf("color spread: min=%u max=%u\n", mn, mx);
    OK(mn > 0 && mx < 3 * mn);

    /* T4: ชายแดนปิด (ไม่มีเพื่อน = 0xFE) */
    OK(mw6_cell_color(0, 0, 0, MW6_DIR_NX) == 0xFE);
    OK(!mw6_gate(0, 0, 0, 0, 0, 0)); /* ไม่ติดกัน */
    OK(mw6_cell_color(0, 0, 0, 9) == 0xFF); /* dir เกิน */

    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
