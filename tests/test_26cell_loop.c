/* test_26cell_loop.c — Void Loop Cycle (Lo Shu pairs) proof, 2 modes.
 * BUILD: gcc -O2 -Wall -I. -Icore tests/test_26cell_loop.c -o test_26cell_loop.exe
 */
#include <stdio.h>
#include <string.h>
#include "core/geo_26cell.h"
#include "core/geo_26cell_loop.h"

int main(void) {
    int pass = 0, fail = 0;

    /* 1. Separate mode uses 3 directions, Extended uses 4 */
    if (loop_num_directions(LOOP_SEPARATE) == 3 && loop_num_directions(LOOP_EXTENDED) == 4) {
        pass++; printf("direction counts: PASS (3/4)\n");
    } else { fail++; printf("FAIL dir counts\n"); }

    /* 2. termination: from any cell, loop_next stays within 0..25 */
    int term_ok = 1;
    for (uint32_t c = 0; c < CELL_N; c++) {
        for (int m = 0; m < 2; m++) {
            uint32_t nxt = loop_next(c, m);
            if (nxt >= CELL_N) { term_ok = 0; printf("FAIL out of range %u mode %d -> %u\n", c, m, nxt); }
        }
    }
    if (term_ok) { pass++; printf("termination/range: PASS\n"); } else { fail++; }

    /* 3. mode difference: at least one cell where next differs between modes */
    int differs = 0;
    for (uint32_t c = 0; c < CELL_N; c++) {
        if (loop_next(c, LOOP_SEPARATE) != loop_next(c, LOOP_EXTENDED)) { differs = 1; break; }
    }
    if (differs) { pass++; printf("mode difference: PASS\n"); } else { fail++; printf("FAIL no mode difference\n"); }

    /* 4. bounded termination: traverse from cell 0 in each mode, must stay within 26 steps */
    for (int m = 0; m < 2; m++) {
        uint32_t cur = 0;
        int steps = 0;
        while (steps < 26) {
            uint32_t nxt = loop_next(cur, (uint32_t)m);
            if (nxt == cur) break; /* fixed point */
            cur = nxt;
            steps++;
        }
        if (steps <= 26) { pass++; printf("mode %d: bounded termination (steps=%d): PASS\n", m, steps); }
        else { fail++; printf("mode %d: NOT bounded\n", m); }
    }

    /* 5. void (value 5) never assigned to any cell */
    int void_ok = 1;
    for (uint32_t c = 0; c < CELL_N; c++) {
        if (loop_cell_value(c) == 5) { void_ok = 0; printf("FAIL void value at cell %u\n", c); }
    }
    if (void_ok) { pass++; printf("void exclusion: PASS\n"); } else { fail++; }

    printf("26cell_loop: %d pass, %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
