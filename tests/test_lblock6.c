/* tests/test_lblock6.c — 3D Hilbert + 6-dir orientation */
#include <stdio.h>
#include "../core/geo_lblock6.h"
#include "../core/mm_wang6.h"

int main(void) {
    int pass = 0, fail = 0;
#define OK(c) do { if (c) { pass++; } else { fail++; printf("FAIL line %d\n", __LINE__); } } while (0)

    /* T1: 8^3=512 cells ครบไม่ซ้ำ */
    static unsigned char seen[8][8][8];
    unsigned n = 0;
    for (uint32_t d = 0; d < 512; d++) {
        uint32_t x, y, z;
        geo_l6_d2xyz(d, 8, &x, &y, &z);
        if (x < 8 && y < 8 && z < 8 && !seen[x][y][z]) { seen[x][y][z] = 1; n++; }
    }
    printf("coverage: %u/512\n", n);
    OK(n == 512);

    /* T2: d ติดกันต้องติดกันใน grid (unit step) */
    unsigned bad = 0;
    for (uint32_t d = 1; d < 512; d++) {
        int dx, dy, dz;
        geo_l6_direction(d, 8, &dx, &dy, &dz);
        int m = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + (dz < 0 ? -dz : dz);
        if (m != 1) bad++;
    }
    printf("non-unit steps: %u/511\n", bad);
    OK(bad == 0);

    /* T3: orientation ครบ 6 ทิศ */
    unsigned mask = 0;
    for (uint32_t d = 1; d < 512; d++) {
        int dx, dy, dz;
        geo_l6_direction(d, 8, &dx, &dy, &dz);
        uint8_t o = geo_l6_orient(dx, dy, dz);
        if (o < 6) mask |= 1u << o;
    }
    printf("orient mask: 0x%x (want 0x3f)\n", mask);
    OK(mask == 0x3f);
    OK(geo_l6_orient(0, 0, 0) == 0xFF);

    /* T5: เดิน Hilbert path 511 ก้าว — wang gate เปิดทุกก้าว + orient ตรงทิศ */
    unsigned shut = 0, mism = 0;
    for (uint32_t d = 1; d < 512; d++) {
        uint32_t x0, y0, z0, x1, y1, z1;
        geo_l6_d2xyz(d - 1u, 8, &x0, &y0, &z0);
        geo_l6_d2xyz(d, 8, &x1, &y1, &z1);
        if (!mw6_gate(x0, y0, z0, x1, y1, z1)) shut++;
        int dx, dy, dz;
        geo_l6_direction(d, 8, &dx, &dy, &dz);
        uint8_t o = geo_l6_orient(dx, dy, dz);
        /* ทิศที่เดินต้องตรงกับ wang dir ของก้าวนั้น */
        int wx = (int)x1 - (int)x0, wy = (int)y1 - (int)y0, wz = (int)z1 - (int)z0;
        uint8_t want = 0xFF;
        if (wx == 1) want = 0; else if (wx == -1) want = 1;
        else if (wy == 1) want = 2; else if (wy == -1) want = 3;
        else if (wz == 1) want = 4; else if (wz == -1) want = 5;
        if (o != want) mism++;
    }
    printf("hilbert walk: shut=%u/511 orient_mismatch=%u/511\n", shut, mism);
    OK(shut == 0 && mism == 0);

    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
