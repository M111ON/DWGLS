/* tools/cube_cylinder_probe.c — Cube-look / Cylinder-manage
 * ════════════════════════════════════════════════════════════
 * User: "ทุกอย่างมันจะดูยุ่งวุ่นวายมากใช่ไหม นี่คือจุดที่ผม implement
 *        cube→cylinder สังเกตุ code ช่วงนั้นจะมีการใช้ spoke
 *        จริงๆมันแค่ดูเป็น cube แต่เราจัดการแบบ cylinder"
 *
 * พิสูจน์: สนามเส้นตรงเดียว (0..20735) มีสอง tiling:
 *   cube view (presentation): 18 tesseracts × 8 cubes × 144 = 20736
 *   cyl  view (management) : 6 cylinders × 3456 = 20736
 *                             (1 cylinder = 6 spokes × 576 = 24 faces × 144)
 *
 * และ "spoke" คือ fingerprint ของการจัดการแบบ cylinder:
 *   addr → spoke = idx % 6 (60° wedge) · slot = idx / 6 (radial)
 *   invert  = (spoke+3)%6 — หน้าตรงข้าม = invert+offset (ยุค geomatrix)
 *   face/unit = slot/64, slot%64 — 8×8 bitboard = 64 units = 1 face
 *   Hilbert 3456 lines = CYL_FULL_N = GN_LINE_MAX (3456)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* ── สนาม / ตัวเลขศักดิ์สิทธิ์ ─────────────────────────────── */
#define FIELD        20736u   /* 12^4 = "10000" ในโลกฐาน 12   */
#define TESSERACTS   18u      /* cube view: 18 tes            */
#define CUBES_PER_T  8u       /* cube view: 8 cube per tes    */
#define CYLINDERS    6u       /* cyl view: 6 cylinders        */
#define CYL_FULL     3456u    /* 1 cylinder = 6×576 = 144×24  */
#define SPOKES       6u       /* 6 LADOS 60°                  */
#define SLOTS_PER_SPOKE 576u  /* 24² = 9×64                   */
#define FACES        9u       /* 8 outer + 1 center           */
#define FACE_UNITS   64u      /* 8² = bitboard ของ geomatrix  */
#define TE_CYCLES    24u      /* 144×24 = 3456 (ThirdEye)     */
#define SLOT144      144u     /* slot ต่อ face/cube           */

static int checks = 0, fails = 0;
#define CHECK(desc, cond) do { \
    checks++; \
    if (cond) { printf("  ✓ %s\n", desc); } \
    else { fails++; printf("  ✗ FAIL: %s\n", desc); } \
} while (0)

int main(void)
{
    printf("═ cube ดู แต่ cylinder จัดการ — probe ═\n\n");

    /* ── A. สนามเดียวกัน สอง tiling ─────────────────────── */
    printf("[A] 20736 = สนามเส้นตรงเดียว สองมุมมอง\n");
    CHECK("cube view: 18 × 8 × 144 == 20736",
          TESSERACTS * CUBES_PER_T * SLOT144 == FIELD);
    CHECK("cyl view: 6 × 3456 == 20736",
          CYLINDERS * CYL_FULL == FIELD);
    CHECK("1 cylinder = 3 tesseracts = 24 cubes × 144",
          CYL_FULL == 3u * CUBES_PER_T * SLOT144 &&
          CYL_FULL == TE_CYCLES * SLOT144);
    CHECK("1 cylinder = 6 spokes × 576",
          CYL_FULL == SPOKES * SLOTS_PER_SPOKE);
    CHECK("576 = 24² = 9 × 64 (face × unit)",
          SLOTS_PER_SPOKE == TE_CYCLES * TE_CYCLES &&
          SLOTS_PER_SPOKE == FACES * FACE_UNITS);
    CHECK("Hilbert 3456 lines = CYL_FULL_N (geomatrix ยุค password)",
          CYL_FULL == 3456u);

    /* ── B. spoke route = bijection บนทั้งสนาม ───────────── */
    printf("\n[B] การจัดการ: addr → (cyl, spoke, slot) + ย้อนกลับ (20736 จุด)\n");
    {
        /* ตาม geo_cylinder.h / geo_net_route:
         *   full_idx = addr % CYL_FULL_N; spoke = full_idx % 6; slot = full_idx / 6
         * สนาม = 6 cylinders × 3456 — cylinder = หน่วย addressing จริง */
        uint32_t bad = 0;
        for (uint32_t idx = 0; idx < FIELD; idx++) {
            uint32_t cyl   = idx / CYL_FULL;
            uint32_t fidx  = idx % CYL_FULL;
            uint8_t  spoke = (uint8_t)(fidx % SPOKES);
            uint16_t slot  = (uint16_t)(fidx / SPOKES);
            uint32_t back  = cyl * CYL_FULL + (uint32_t)slot * SPOKES + spoke;
            if (back != idx || spoke >= SPOKES || slot >= SLOTS_PER_SPOKE)
                bad++;
        }
        CHECK("∀ idx: (cyl=idx/3456, spoke=(idx%3456)%6, slot=(idx%3456)/6) roundtrip",
              bad == 0);
    }

    /* ── C. invert = หน้าตรงข้าม (spoke+3)%6 ─────────────── */
    printf("\n[C] invert pair — 'หน้าตรงข้ามเป็น invert+offset' (ยุค geomatrix)\n");
    CHECK("invert: 0↔3 1↔4 2↔5 (ทุกคู่ต่างกัน, invert² = id)",
          (0+3)%6==3 && (3+3)%6==0 &&
          (1+3)%6==4 && (4+3)%6==1 &&
          (2+3)%6==5 && (5+3)%6==2);
    {
        /* 3 spokes หน้า = ครึ่งสนามพอดี (visible half) */
        uint32_t half = 0;
        for (uint32_t idx = 0; idx < FIELD; idx++)
            if ((idx % SPOKES) < 3u) half++;
        CHECK("spokes 0..2 = ครึ่งสนามเป๊ะ (10368 = 20736/2)", half == FIELD/2);
    }

    /* ── D. face/unit: slot → 9 faces × 64 units ─────────── */
    printf("\n[D] slot → (face 0..8, unit 0..63) — 8×8 bitboard = 1 face\n");
    {
        uint32_t bad = 0, center = 0;
        for (uint32_t slot = 0; slot < SLOTS_PER_SPOKE; slot++) {
            uint8_t face = (uint8_t)(slot / FACE_UNITS);
            (void)FACE_UNITS;   /* unit = slot % 64 ตรวจผ่าน face อยู่แล้ว */
            if (face >= FACES) bad++;
            if (slot >= 512u) center++;   /* center face = slot 512..575 */
        }
        CHECK("slot/64 → face 0..8 ครบ (bad=0)", bad == 0);
        CHECK("center face = slot 512..575 (64 units, 9th face)",
              center == FACE_UNITS);
    }

    /* ── E. cylinder = 24 faces × 144 (ThirdEye closure) ─── */
    printf("\n[E] 1 cylinder = 24 face/cube × 144 slots (3456/144 = 24)\n");
    {
        uint32_t bad = 0;
        for (uint32_t idx = 0; idx < CYL_FULL; idx++) {
            uint8_t  face = (uint8_t)((idx / SLOT144) % TE_CYCLES);
            uint16_t s144 = (uint16_t)(idx % SLOT144);
            uint32_t back = (uint32_t)face * SLOT144 + s144;
            if (back != idx) bad++;
        }
        CHECK("∀ idx ∈ [0,3456): (face24 = idx/144, s144 = idx%144) roundtrip",
              bad == 0);
        CHECK("20736 = 144 faces × 144 slots (24×6 cylinders)",
              FIELD == TE_CYCLES * CYLINDERS * SLOT144);
    }

    /* ── F. 24 faces ของ cylinder = 3 tesseracts (cube view) ── */
    printf("\n[F] cylinder ↔ cube view: 24 face/cube ต่อ cylinder\n");
    {
        /* cylinder c → tesseract 3c, 3c+1, 3c+2 (8 cubes ต่อ tes) */
        uint32_t bad = 0;
        for (uint32_t c = 0; c < CYLINDERS; c++) {
            for (uint32_t f = 0; f < TE_CYCLES; f++) {
                uint32_t cyl_idx = c * CYL_FULL + f * SLOT144;
                /* มุมมอง cube: t = (c*3 + f/8), cube = f%8 */
                uint32_t t = c * 3u + f / CUBES_PER_T;
                uint32_t cu = f % CUBES_PER_T;
                uint32_t cube_idx = (t * CUBES_PER_T + cu) * SLOT144;
                if (cyl_idx != cube_idx) bad++;
            }
        }
        CHECK("cyl (c, face f) ≡ cube (tes c×3+f/8, cube f%8) — ตำแหน่งเดียวกัน",
              bad == 0);
    }

    printf("\n══════════ %d/%d PASS ══════════\n", checks - fails, checks);
    return fails ? 1 : 0;
}
