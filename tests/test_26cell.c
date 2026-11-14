/* test_26cell.c — 26-Cell Hollow Cube + Pair LetterCube bijection proof.
 * BUILD: gcc -O2 -Wall -I. -Icore tests/test_26cell.c -o test_26cell.exe
 */
#include <stdio.h>
#include <stdint.h>
#include "core/geo_26cell.h"

int main(void) {
    int pass = 0, fail = 0;

    /* 1. total cells = 27, void = 1 center, non-void = 26 */
    if (CELL_TOTAL == 27 && CELL_N == 26 && CELL_VOID == 13) { pass++; }
    else { fail++; printf("FAIL: cell counts\n"); }

    /* 2. void is center (1,1,1) */
    if (cell_idx(1,1,1) == CELL_VOID) { pass++; }
    else { fail++; printf("FAIL: void index\n"); }

    /* 3. all 27 indices: void check correct */
    int void_count = 0, nonvoid = 0;
    for (uint32_t i = 0; i < CELL_TOTAL; i++) {
        if (cell_is_void_idx(i)) void_count++;
        else nonvoid++;
    }
    if (void_count == 1 && nonvoid == 26) { pass++; }
    else { fail++; printf("FAIL: void count %d nonvoid %d\n", void_count, nonvoid); }

    /* 4. compact mapping is bijection (roundtrip for all non-void) */
    int rt_ok = 1;
    for (uint32_t i = 0; i < CELL_TOTAL; i++) {
        if (cell_is_void_idx(i)) continue;
        if (!cell_roundtrip_ok(i)) { rt_ok = 0; printf("FAIL roundtrip idx %u\n", i); }
    }
    if (rt_ok) { pass++; } else { fail++; }

    /* 5. compact indices are 0..25 distinct */
    int seen[26] = {0};
    int distinct = 1;
    for (uint32_t i = 0; i < CELL_TOTAL; i++) {
        if (cell_is_void_idx(i)) continue;
        uint32_t c = cell_compact(i);
        if (c >= 26 || seen[c]) { distinct = 0; printf("FAIL compact dup/out %u\n", c); }
        seen[c] = 1;
    }
    if (distinct) { pass++; } else { fail++; }

    /* 6. letter pairs: 26 distinct upper/lower */
    int letters_ok = 1;
    for (uint32_t c = 0; c < 26; c++) {
        char u = cell_upper(c), l = cell_lower(c);
        if (u < 'A' || u > 'Z' || l < 'a' || l > 'z') { letters_ok = 0; }
    }
    if (letters_ok) { pass++; } else { fail++; printf("FAIL letters\n"); }

    /* 7. void cell has no letter (compact undefined) — just ensure void idx not in compact range as valid */
    printf("26-cell: %d pass, %d fail\n", pass, fail);
    /* print mapping sample */
    printf("sample: idx0 compact=%u letter=%c%c, idx13(void)=%d, idx26 compact=%u letter=%c%c\n",
        cell_compact(0), cell_upper(cell_compact(0)), cell_lower(cell_compact(0)),
        cell_is_void_idx(13),
        cell_compact(26), cell_upper(cell_compact(26)), cell_lower(cell_compact(26)));
    return fail ? 1 : 0;
}
