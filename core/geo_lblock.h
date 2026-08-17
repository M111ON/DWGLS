/* geo_lblock.h — L-Block: summon from Hilbert position (extension of geo_jump)
 * ═══════════════════════════════════════════════════════════════════════════
 * C port ของ lblock_from_hilbert (FGLS_new/collection/colab_bench/geo_frame_seek.py,
 * commit e8cb122 "Capture Twin + L-block" — ตัวเดียวกับชุด 72 free centroids).
 *
 * หลักการ (user design — ใช้ร่วมกับ RDH):
 *   RDH วิ่งตาม tensor → ได้พิกัดที่หยุดนิ่ง (block, from)
 *   → กางออก (unfold ลง grid) → ครอบด้วย L-block
 *   → ส่งเข้า storage → ปัก address
 *
 *   L-block บอกว่า container จะวางหันทางไหน (rotation 0..3 กำหนดโดย
 *   ทิศทางที่ Hilbert curve เดินเข้าตำแหน่ง d จาก d-1) และการันตีว่า
 *   "ตัดชิ้นนี้ไปใส่ hilbert container วางได้ fit แน่นอน":
 *   - address เดิม → ทิศเดิม → rotation เดิม → deterministic
 *   - 4 cells ไม่ซ้ำกันเสมอ (piece ไม่ collapse)
 *   - wrap modulo grid → ทุก address วางลง n×n container ได้ครบ 4 ช่อง
 *
 * "Summon" เหมือน RDH: ฟังก์ชันเดียวจาก address d — ไม่มี lookup table,
 * เลขคณิตล้วน, deterministic, int ล้วน (กฎ no float ของระบบ)
 *
 * API:
 *   geo_lb_d2xy(d, n, &x, &y)          inverse Hilbert (d → grid coord)
 *   geo_lb_direction(d, n, &dx, &dy)   ทิศ curve เข้าตำแหน่ง d จาก d-1
 *   geo_lb_rotation(dx, dy)            ทิศ → rotation 0..3 (ขวา/ลง/ซ้าย/ขึ้น)
 *   geo_lb_shape(x, y, rot, cells[4])  rotation → 4 cells รูป L
 *   geo_lb_from_hilbert(d, n, ...)     THE SUMMON — address → piece
 *   geo_lb_slot(d, n)                  anchor slot บน container (mod n)
 *   geo_lb_fits_grid(d, n)             fit guarantee ต่อ address
 *
 * Dependencies: <stdint.h> เท่านั้น — self-contained
 */

#ifndef GEO_LBLOCK_H
#define GEO_LBLOCK_H

#include <stdint.h>

/* ── inverse Hilbert d→xy (order n; n ต้องเป็น power of 2) ── */
static inline void geo_lb_d2xy(uint32_t d, uint32_t n, uint32_t *x, uint32_t *y) {
    uint32_t hx = 0, hy = 0, s = 1;
    while (s < n) {
        uint32_t rx = (d >> 1) & 1u;
        uint32_t ry = (d ^ rx) & 1u;
        if (ry == 0) {
            if (rx == 1) { hx = s - 1u - hx; hy = s - 1u - hy; }
            uint32_t t = hx; hx = hy; hy = t;
        }
        hx += rx * s;
        hy += ry * s;
        d >>= 2;
        s <<= 1;
    }
    *x = hx; *y = hy;
}

/* ── ทิศทางที่ curve เดินเข้าตำแหน่ง d จาก d-1 (wrap ข้ามรอบ) ──
 * คืนหนึ่งใน (1,0), (0,1), (-1,0), (0,-1) — normalize ขอบ grid */
static inline void geo_lb_direction(uint32_t d, uint32_t n,
                                    int32_t *dx, int32_t *dy) {
    uint32_t d_prev = (d + n * n - 1u) % (n * n);
    uint32_t xp, yp, x, y;
    geo_lb_d2xy(d_prev, n, &xp, &yp);
    geo_lb_d2xy(d, n, &x, &y);
    int32_t vx = (int32_t)x - (int32_t)xp;
    int32_t vy = (int32_t)y - (int32_t)yp;
    if (vx > 1) vx = -1;
    if (vx < -1) vx = 1;
    if (vy > 1) vy = -1;
    if (vy < -1) vy = 1;
    *dx = vx; *dy = vy;
}

/* ── ทิศ → rotation: ขวา→0 ลง→1 ซ้าย→2 ขึ้น→3 (เทียบ Python direction_to_rotation) ── */
static inline uint32_t geo_lb_rotation(int32_t dx, int32_t dy) {
    if (dx == 1  && dy == 0) return 0u;
    if (dx == 0  && dy == 1) return 1u;
    if (dx == -1 && dy == 0) return 2u;
    if (dx == 0  && dy == -1) return 3u;
    return 0u; /* fallback — ไม่ควรเกิด */
}

/* ── rotation → 4 cells รูป L (base: XXX+X ชี้ขวา; หมุนตาม rot) ──
 * cells[i][0]=x, cells[i][1]=y — int32 เพราะ rot 1/3 ทำได้ x−1 (ติดขอบ grid) */
static inline void geo_lb_shape(uint32_t x, uint32_t y, uint32_t rot,
                                int32_t cells[4][2]) {
    static const int32_t base[4][2] = {{0,0},{1,0},{2,0},{2,1}};
    for (int i = 0; i < 4; i++) {
        int32_t bx = base[i][0], by = base[i][1];
        int32_t cx, cy;
        switch (rot) {
            case 1:  cx = (int32_t)x - by;      cy = (int32_t)y + bx;      break;
            case 2:  cx = (int32_t)x - bx;      cy = (int32_t)y - by;      break;
            case 3:  cx = (int32_t)x + by;      cy = (int32_t)y - bx;      break;
            default: cx = (int32_t)x + bx;      cy = (int32_t)y + by;      break;
        }
        cells[i][0] = cx;
        cells[i][1] = cy;
    }
}

/* ── THE SUMMON — address d → piece: cells(4) + rotation + ทิศทาง
 * address เดิม → ทิศเดิม → rotation เดิม → deterministic (การันตี fit) */
static inline void geo_lb_from_hilbert(uint32_t d, uint32_t n,
                                       int32_t cells[4][2],
                                       uint32_t *rot_out,
                                       int32_t *dx_out, int32_t *dy_out) {
    uint32_t x, y, rot;
    int32_t dx, dy;
    geo_lb_d2xy(d, n, &x, &y);
    geo_lb_direction(d, n, &dx, &dy);
    rot = geo_lb_rotation(dx, dy);
    geo_lb_shape(x, y, rot, cells);
    if (rot_out) *rot_out = rot;
    if (dx_out)  *dx_out  = dx;
    if (dy_out)  *dy_out  = dy;
}

/* ── anchor slot ของ piece บน n×n container = ตำแหน่งที่ summon (mod grid) ──
 * ตัดชิ้นที่ d → ปักที่ slot d → อ่านกลับด้วยการ re-summon d เดิม */
static inline uint32_t geo_lb_slot(uint32_t d, uint32_t n) {
    return d % (n * n);
}

/* ── FIT GUARANTEE — piece ที่ summon จาก d วางบน n×n container (wrap mod)
 * ต้องได้ 4 ช่องที่ไม่ซ้ำกัน (ไม่มี overlap/collapse) — "วางได้ fit แน่นอน" */
static inline int geo_lb_fits_grid(uint32_t d, uint32_t n) {
    int32_t cells[4][2];
    uint32_t rot; int32_t dx, dy;
    uint32_t seen[4];
    geo_lb_from_hilbert(d, n, cells, &rot, &dx, &dy);
    (void)rot; (void)dx; (void)dy;
    for (int i = 0; i < 4; i++) {
        uint32_t cx = (uint32_t)cells[i][0] % n;   /* int32 ลบ → wrap ถูกต้อง (Python %) */
        uint32_t cy = (uint32_t)cells[i][1] % n;
        uint32_t slot = cy * n + cx;
        for (int j = 0; j < i; j++)
            if (seen[j] == slot) return 0;
        seen[i] = slot;
    }
    return 1;
}

#endif /* GEO_LBLOCK_H */
