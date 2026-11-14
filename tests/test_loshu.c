/* test_loshu.c — Lo Shu 3x3 projection proof.
 * BUILD: gcc -O2 -Wall -I. -Icore tests/test_loshu.c -o test_loshu.exe
 */
#include <stdio.h>
#include <string.h>
#include "core/geo_loshu.h"

int main(void) {
    int pass = 0, fail = 0;

    /* 1. all 14 pyramid points project to valid grid 0..8 */
    int valid = 1;
    for (uint32_t i = 0; i < PYR_POINTS; i++) {
        uint8_t g = loshu_project(i);
        if (g >= LOSHU_GRID) { valid = 0; printf("FAIL invalid grid %u -> %u\n", i, g); }
    }
    if (valid) { pass++; printf("projection validity: PASS\n"); } else { fail++; }

    /* 2. surjective: all 9 grid positions used */
    char seen[9] = {0};
    for (uint32_t i = 0; i < PYR_POINTS; i++) seen[loshu_project(i)] = 1;
    int surj = 1;
    for (int g = 0; g < 9; g++) if (!seen[g]) { surj = 0; printf("FAIL grid %d unused\n", g); }
    if (surj) { pass++; printf("surjectivity: PASS (9/9)\n"); } else { fail++; }

    /* 3. grid identity: project on grid = itself (idempotent view) */
    int idem = 1;
    for (uint8_t g = 0; g < 9; g++) {
        /* grid positions are fixed points: loshu_project of a grid index treated as pyramid? 
           We check loshu_grid_id(g)==g */
        if (loshu_grid_id(g) != g) { idem = 0; }
    }
    if (idem) { pass++; printf("grid identity: PASS\n"); } else { fail++; }

    /* 4. center (index 4) receives apex and center point */
    if (loshu_project(0) == 4 && loshu_project(13) == 4) { pass++; printf("center receives apex+center: PASS\n"); }
    else { fail++; printf("FAIL center\n"); }

    printf("loshu: %d pass, %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
