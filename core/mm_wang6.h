/* core/mm_wang6.h — 6-direction Wang colors for 3D maze cells.
 *
 * ทิศ: +X=0, -X=1, +Y=2, -Y=3, +Z=4, -Z=5. opposite = dir^1.
 * สีของ face คำนวณจาก canonical face key (min-cell coords + axis) —
 * สองฝั่งเห็นสีเดียวกันอัตโนมัติ (Wang rule ตรงโดย construction),
 * แต่ต่าง faces ต่างสี (hash % 8) → maze gate มีความหมาย.
 * Gate: เปิด iff สีตรง (default), override ปิด/เปิดได้เหมือน mm_wang.h.
 *
 * Header-only. Depends: stdint only.
 */
#ifndef MM_WANG6_H
#define MM_WANG6_H

#include <stdint.h>

#define MW6_COLORS 8u
#define MW6_DIR_PX 0u
#define MW6_DIR_NX 1u
#define MW6_DIR_PY 2u
#define MW6_DIR_NY 3u
#define MW6_DIR_PZ 4u
#define MW6_DIR_NZ 5u

static inline uint8_t mw6_opp(uint8_t d) { return (uint8_t)(d ^ 1u); }

/* canonical face key → color. (x,y,z) = cell ฝั่ง min ของแกน axis (0,1,2). */
static inline uint8_t mw6_face_color(unsigned x, unsigned y, unsigned z, unsigned axis) {
    uint32_t h = (uint32_t)(x * 5u + y * 11u + z * 7u + axis * 13u);
    h ^= h >> 4;
    h *= 0x45d9f3bu;
    h ^= h >> 4;
    return (uint8_t)(h % MW6_COLORS);
}

/* สีของ cell (x,y,z) ด้าน dir: แปลงเป็น canonical face ก่อน */
static inline uint8_t mw6_cell_color(unsigned x, unsigned y, unsigned z, uint8_t dir) {
    if (dir > 5) return 0xFF;
    unsigned axis = dir / 2u;
    unsigned cx = x, cy = y, cz = z;
    if (dir & 1u) { /* ด้านลบ: face key อยู่ที่ cell นี้ (min ฝั่ง -) */
        /* face ระหว่าง cell-1 กับ cell → min = cell-1 */
        if (axis == 0) { if (x == 0) return 0xFE; cx = x - 1; }
        if (axis == 1) { if (y == 0) return 0xFE; cy = y - 1; }
        if (axis == 2) { if (z == 0) return 0xFE; cz = z - 1; }
    }
    return mw6_face_color(cx, cy, cz, axis);
}

/* gate ระหว่าง cell คู่ติดกัน: เปิด iff สีฝั่งตรงกัน (0xFE ชายแดน = ปิด) */
static inline int mw6_gate(unsigned x1, unsigned y1, unsigned z1,
                           unsigned x2, unsigned y2, unsigned z2) {
    int dx = (int)x2 - (int)x1, dy = (int)y2 - (int)y1, dz = (int)z2 - (int)z1;
    uint8_t d;
    if (dx == 1 && !dy && !dz) d = MW6_DIR_PX;
    else if (dx == -1 && !dy && !dz) d = MW6_DIR_NX;
    else if (dy == 1 && !dx && !dz) d = MW6_DIR_PY;
    else if (dy == -1 && !dx && !dz) d = MW6_DIR_NY;
    else if (dz == 1 && !dx && !dy) d = MW6_DIR_PZ;
    else if (dz == -1 && !dx && !dy) d = MW6_DIR_NZ;
    else return 0;
    uint8_t a = mw6_cell_color(x1, y1, z1, d);
    uint8_t b = mw6_cell_color(x2, y2, z2, mw6_opp(d));
    if (a >= 0xFE || b >= 0xFE) return 0;
    return a == b;
}

#endif /* MM_WANG6_H */
