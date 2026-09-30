/* core/geo_lblock6.h — L-Block 3D: Hilbert d→xyz + 6-direction entry orientation.
 *
 * ต่อจาก geo_lblock.h (2D, rotation 0-3) ขึ้น 3D ด้วย Butz Hilbert decode:
 *   geo_l6_d2xyz(d, n, &x, &y, &z)   d → grid coord (n = power of 2)
 *   geo_l6_direction(d, n, &dx..)    ทิศ curve เข้า d จาก d-1 (6 ทิศ)
 *   geo_l6_orient(dx,dy,dz)          ทิศ → orientation 0-5 (+X,-X,+Y,-Y,+Z,-Z)
 * ผ่าน mw6 (mm_wang6.h): orientation นี้ = ด้านที่ L-piece หันออก = wang dir
 * เดียวกัน → L-Block วางบน maze ได้พร้อม gate 6 ทิศทันที.
 *
 * Header-only, int-only, no malloc. Depends: stdint only.
 */
#ifndef GEO_LBLOCK6_H
#define GEO_LBLOCK6_H

#include <stdint.h>

/* ── Skilling 3D Hilbert d→xyz (n = 2^p) ── */
static inline void geo_l6_d2xyz(uint32_t d, uint32_t n,
                                uint32_t *x, uint32_t *y, uint32_t *z) {
    unsigned p = 0;
    while ((1u << p) < n) p++;
    unsigned N = 3;
    /* transpose: กระจาย bits ของ d เป็น 3 แกน */
    uint32_t xx[3] = {0, 0, 0};
    for (unsigned i = 0; i < p * N; i++) {
        unsigned bit = (d >> i) & 1u;
        unsigned axis = (p * N - 1u - i) % N;
        unsigned pos = i / N;
        xx[axis] |= bit << pos;
    }
    /* Gray decode */
    uint32_t t = xx[N - 1] >> 1;
    for (int i = (int)N - 1; i > 0; i--) xx[i] ^= xx[i - 1];
    xx[0] ^= t;
    /* Undo excess work */
    uint32_t Q = 2;
    while (Q != (1u << p)) {
        uint32_t P = Q - 1u;
        for (int i = (int)N - 1; i >= 0; i--) {
            if (xx[i] & Q) {
                xx[0] ^= P;
            } else {
                uint32_t tt = (xx[0] ^ xx[i]) & P;
                xx[0] ^= tt;
                xx[i] ^= tt;
            }
        }
        Q <<= 1;
    }
    if (x) *x = xx[2];
    if (y) *y = xx[1];
    if (z) *z = xx[0];
}

/* ── ทิศเข้า d จาก d-1 (d>0) ── */
static inline void geo_l6_direction(uint32_t d, uint32_t n,
                                    int *dx, int *dy, int *dz) {
    uint32_t x0, y0, z0, x1, y1, z1;
    geo_l6_d2xyz(d - 1u, n, &x0, &y0, &z0);
    geo_l6_d2xyz(d, n, &x1, &y1, &z1);
    if (dx) *dx = (int)x1 - (int)x0;
    if (dy) *dy = (int)y1 - (int)y0;
    if (dz) *dz = (int)z1 - (int)z0;
}

/* ── ทิศ → orientation 0-5 (ตรงกับ MW6_DIR_*) ── */
static inline uint8_t geo_l6_orient(int dx, int dy, int dz) {
    if (dx == 1 && !dy && !dz) return 0;
    if (dx == -1 && !dy && !dz) return 1;
    if (dy == 1 && !dx && !dz) return 2;
    if (dy == -1 && !dx && !dz) return 3;
    if (dz == 1 && !dx && !dy) return 4;
    if (dz == -1 && !dx && !dy) return 5;
    return 0xFF;
}

#endif /* GEO_LBLOCK6_H */
