/* test_geo_lblock.c — L-Block: summon + fit guarantee + RDH chain
 * ═══════════════════════════════════════════════════════════════════════════
 * C port ของ lblock_from_hilbert (FGLS_new geo_frame_seek.py) — พิสูจน์:
 *
 *   A. port ตรงกับ Python verify (deterministic, 4 cells ไม่ซ้ำ, ครบ 4 rotation)
 *   B. fit guarantee — ทุก address บน n×n container (wrap mod) วางได้ 4 ช่อง
 *      ไม่ชนกัน — "ตัดชิ้นนี้ไปใส่ hilbert container วางได้ fit แน่นอน"
 *   C. CHAIN (user design): RDH วิ่งตาม tensor → พิกัดที่หยุดนิ่ง (block, from)
 *      → กางออก (unfold ลง grid ผ่าน wedge digit) → ครอบด้วย L-block (rotation)
 *      → ส่งเข้า storage → ปัก address → อ่านกลับ lossless
 *   D. negative — เปลี่ยน scale (เสาเข็ม) → คนละ slot → mismatch (address คือหลัก)
 *
 * หลักการ: address เดิม → ทิศเดิม → rotation เดิม → deterministic — และ
 * address IS data (rdh_decompose กู้ (block, from) กลับได้)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore \
 *        -o build/test-geo_lblock tests/test_geo_lblock.c -lm
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../core/geo_rdh_addr.h"
#include "../core/geo_lblock.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── A. port verify (เทียบ Python lblock_verify) ── */
static void test_port(void) {
    printf("\nA. L-block port — deterministic rotation from Hilbert position\n");

    /* A1: direction → rotation ครบ 4 ทิศ */
    {
        uint32_t rots = 0;
        rots |= 1u << geo_lb_rotation( 1,  0);
        rots |= 1u << geo_lb_rotation( 0,  1);
        rots |= 1u << geo_lb_rotation(-1,  0);
        rots |= 1u << geo_lb_rotation( 0, -1);
        CHECK(1, "direction→rotation ครอบครบ 4 ทิศ", rots == 0xFu);
    }

    /* A2: แต่ละ rotation ได้ 4 cells ไม่ซ้ำ (ตำแหน่งกลาง grid (4,4)) */
    {
        int ok = 1;
        for (uint32_t r = 0; r < 4; r++) {
            int32_t cells[4][2];
            geo_lb_shape(4, 4, r, cells);
            int dup = 0;
            for (int i = 0; i < 4 && !dup; i++)
                for (int j = i + 1; j < 4; j++)
                    if (cells[i][0] == cells[j][0] && cells[i][1] == cells[j][1]) dup = 1;
            if (dup) ok = 0;
        }
        CHECK(2, "ทุกรูป rotation ได้ 4 cells ไม่ซ้ำ", ok);
    }

    /* A3+A4: ทุกตำแหน่ง (n=8) → rot ใช้ได้ + deterministic */
    {
        int ok_pos = 1, ok_det = 1;
        uint32_t rot_count[4] = {0,0,0,0};
        for (uint32_t d = 0; d < 64; d++) {
            int32_t c1[4][2], c2[4][2];
            uint32_t r1, r2; int32_t dx, dy;
            geo_lb_from_hilbert(d, 8, c1, &r1, &dx, &dy);
            geo_lb_from_hilbert(d, 8, c2, &r2, &dx, &dy);
            if (r1 > 3) ok_pos = 0;
            if (r1 != r2) ok_det = 0;
            rot_count[r1]++;
        }
        CHECK(3, "ทุก 64 ตำแหน่งได้ rotation ใช้ได้", ok_pos);
        CHECK(4, "deterministic — address เดิม → rotation เดิมเสมอ", ok_det);
        CHECK(5, "rotation กระจายครบ 4 ทิศ (n=8: แต่ละ ≥ 10)",
              rot_count[0] >= 10 && rot_count[1] >= 10 &&
              rot_count[2] >= 10 && rot_count[3] >= 10);
        printf("        rot dist (n=8): [%u %u %u %u]\n",
               rot_count[0], rot_count[1], rot_count[2], rot_count[3]);
    }
}

/* ── B. fit guarantee — ทุก address วาง fit บน n×n container ── */
static void test_fit(void) {
    printf("\nB. fit guarantee — ตัดชิ้นนี้ใส่ hilbert container วางได้ fit แน่นอน\n");
    for (int n = 8; n <= 16; n <<= 1) {
        uint32_t total = (uint32_t)n * (uint32_t)n;
        int bad = 0, in_bounds = 0;
        for (uint32_t d = 0; d < total; d++) {
            if (!geo_lb_fits_grid(d, (uint32_t)n)) bad++;
            int32_t cells[4][2];
            uint32_t rot; int32_t dx, dy;
            geo_lb_from_hilbert(d, (uint32_t)n, cells, &rot, &dx, &dy);
            int ib = 1;
            for (int i = 0; i < 4; i++)
                if (cells[i][0] < 0 || cells[i][0] >= n || cells[i][1] < 0 || cells[i][1] >= n) ib = 0;
            if (ib) in_bounds++;
        }
        CHECK(6 + (n == 16),
              n == 8 ? "n=8: fit ครบ 64/64 (4 wrapped slots ไม่ชน)" :
                       "n=16: fit ครบ 256/256 (4 wrapped slots ไม่ชน)",
              bad == 0);
        printf("        n=%d: in-bounds ตรงๆ %d/%u · ที่เหลือ wrap (mod) ยัง fit\n",
               n, in_bounds, total);
    }
}

/* ── C. RDH chain — วิ่งตาม tensor → พิกัดหยุดนิ่ง → กางออก → L-block → storage ── */
static void test_chain(void) {
    printf("\nC. CHAIN — RDH วิ่งตาม tensor → พิกัดหยุดนิ่ง → กางออก → L-block → storage → ปัก address\n");
    enum { N = 256, CELL = 64, GRID = 16, SLOTS = 256 };

    /* container: GRID×GRID slots × CELL bytes */
    uint8_t container[SLOTS][CELL];
    memset(container, 0, sizeof(container));

    /* RDH วิ่งตาม tensor: chunk i → address = rdh_addr(block=i/256, from=i%256) = i
       (mixed-radix ตรงตัว — decompose กู้พิกัดคืน) */
    uint32_t planted_addr[N], planted_rot[N];
    int ok_lossless = 1, ok_slots = 1, ok_rot = 1;

    for (int i = 0; i < N; i++) {
        /* พิกัดที่หยุดนิ่ง — (block, from) ของ chunk i */
        uint32_t block = (uint32_t)i / 256u, from = (uint32_t)i % 256u;
        uint64_t addr = rdh_addr(block, from);

        /* กางออก: wedge digit = ตำแหน่งบน 16×16 Hilbert container */
        uint32_t d = (uint32_t)(addr % (GRID * GRID));

        /* ครอบด้วย L-block: summon → rotation (บอกว่าวางหันทางไหน) */
        int32_t cells[4][2];
        uint32_t rot; int32_t dx, dy;
        geo_lb_from_hilbert(d, GRID, cells, &rot, &dx, &dy);

        /* ส่งเข้า storage ที่ slot = ตำแหน่งที่ summon */
        uint32_t slot = geo_lb_slot(d, GRID);
        if (slot != d) ok_slots = 0;
        uint8_t chunk[CELL];
        for (int b = 0; b < CELL; b++)
            chunk[b] = (uint8_t)((i * 31 + b * 7 + 13) & 0xFF);  /* data จำลอง */
        memcpy(container[slot], chunk, CELL);

        planted_addr[i] = (uint32_t)addr;
        planted_rot[i]  = rot;
    }

    /* อ่านกลับ: re-summon จาก address → slot เดิม → เปรียบเทียบ byte-for-byte */
    for (int i = 0; i < N; i++) {
        uint32_t block, from;
        rdh_decompose(planted_addr[i], &block, &from);
        uint32_t d = (uint32_t)(planted_addr[i] % (GRID * GRID));
        uint32_t slot = geo_lb_slot(d, GRID);
        uint8_t expect[CELL];
        for (int b = 0; b < CELL; b++)
            expect[b] = (uint8_t)((i * 31 + b * 7 + 13) & 0xFF);
        if (memcmp(container[slot], expect, CELL) != 0) ok_lossless = 0;

        /* determinism ที่ระดับ address: re-summon → rotation เดิม */
        int32_t cells[4][2];
        uint32_t rot; int32_t dx, dy;
        geo_lb_from_hilbert(d, GRID, cells, &rot, &dx, &dy);
        if (rot != planted_rot[i]) ok_rot = 0;
    }

    CHECK(8, "C1: lossless — 256 chunks × 64B อ่านกลับ byte-for-byte ผ่าน chain", ok_lossless);
    CHECK(9, "C2: anchor slot = ตำแหน่ง summon (d)", ok_slots);
    CHECK(10, "C3: deterministic — re-summon จาก address ได้ rotation เดิม", ok_rot);

    /* address IS data: decompose กู้ (block, from) กลับจาก address ที่ปัก */
    {
        int ok_dec = 1;
        for (int i = 0; i < N; i++) {
            uint32_t block, from;
            rdh_decompose(planted_addr[i], &block, &from);
            if (block != (uint32_t)i / 256u || from != (uint32_t)i % 256u) ok_dec = 0;
        }
        CHECK(11, "C4: address IS data — decompose กู้ (block, from) ตรงเป๊ะ", ok_dec);
    }

    /* rotation กระจายครบ 4 ทิศบน 256 chunks */
    {
        uint32_t rc[4] = {0,0,0,0};
        for (int i = 0; i < N; i++) rc[planted_rot[i]]++;
        CHECK(12, "C5: rotation ครบ 4 ทิศตลอดทั้ง tensor (n=16)", rc[0] && rc[1] && rc[2] && rc[3]);
        printf("        rot dist (n=16, %d chunks): [%u %u %u %u]\n",
               N, rc[0], rc[1], rc[2], rc[3]);
    }
}

/* ── E. bookmark + expansion — กลับมาทิศเดิม ไม่มั่ว + รู้ว่าจะเดินยังไง ── */
static void test_bookmark_expand(void) {
    printf("\nE. bookmark + expand — วางคืนทิศเดิมเสมอ + รู้ว่าจะเดินยังไง\n");
    enum { N = 16, GRID = 16 };

    /* E1: bookmark — address เดียว → cells เหมือนเดิมทุก bit (ไม่ใช่มั่วทิศ) */
    {
        int ok = 1;
        for (uint32_t d = 0; d < 64; d++) {
            int32_t a[4][2], b[4][2];
            uint32_t ra, rb; int32_t dx, dy;
            geo_lb_from_hilbert(d, GRID, a, &ra, &dx, &dy);
            geo_lb_from_hilbert(d, GRID, b, &rb, &dx, &dy);
            if (ra != rb || memcmp(a, b, sizeof(a)) != 0) ok = 0;
        }
        CHECK(14, "E1: re-summon → rotation + 4 cells เหมือนเดิมทุก bit (วางคืนทิศเดิม)", ok);
    }

    /* E2: expansion set — piece = anchor + 3 เพื่อนบ้าน · ทั้งหมดเป็น slot ต่างกันบน grid
          (expand รอบๆ ได้โดยไม่ชน anchor — เซลล์รอบข้างมากับการ summon ฟรี) */
    {
        int ok = 1;
        for (uint32_t d = 0; d < N; d++) {
            int32_t cells[4][2];
            uint32_t rot; int32_t dx, dy;
            geo_lb_from_hilbert(d, GRID, cells, &rot, &dx, &dy);
            uint32_t seen[4];
            for (int i = 0; i < 4; i++) {
                uint32_t cx = (uint32_t)cells[i][0] % GRID;
                uint32_t cy = (uint32_t)cells[i][1] % GRID;
                seen[i] = cy * GRID + cx;
            }
            for (int i = 0; i < 4 && ok; i++)
                for (int j = i + 1; j < 4; j++)
                    if (seen[i] == seen[j]) ok = 0;
        }
        CHECK(15, "E2: expand set = anchor + 3 เพื่อนบ้าน เป็น slot ต่างกันครบ (ขยายรอบๆ ได้)", ok);
    }

    /* E3: serial walk — จาก bookmark เดิน d → d+1 → d+2 … (ต่อ Hilbert curve)
          = รู้ว่าจะเดินยังไง: deterministic + slot ใหม่ไม่ซ้ำในหน้าต่าง */
    {
        int ok_walk = 1, ok_det = 1;
        for (uint32_t start = 0; start < 8; start++) {
            uint32_t slots[16];
            for (int k = 0; k < 16; k++) {
                uint32_t d = (start + (uint32_t)k) % (GRID * GRID);
                slots[k] = geo_lb_slot(d, GRID);
            }
            for (int i = 0; i < 16 && ok_walk; i++)
                for (int j = i + 1; j < 16; j++)
                    if (slots[i] == slots[j]) ok_walk = 0;
            /* เดินซ้ำ → เหมือนเดิม */
            for (int k = 0; k < 16; k++) {
                uint32_t d = (start + (uint32_t)k) % (GRID * GRID);
                if (geo_lb_slot(d, GRID) != slots[k]) ok_det = 0;
            }
        }
        CHECK(16, "E3: เดินต่อ Hilbert curve (d→d+1→…) deterministic + slot ไม่ซ้ำในหน้าต่าง 16",
              ok_walk && ok_det);
    }

    /* E4: ทิศที่ d+1 ต้องเท่ากับ displacement จริงจากตำแหน่ง d ไป d+1 —
          รู้เส้นทางล่วงหน้าทุก step จาก address (นิยามตรงกับเรขาคณิต) */
    {
        int ok = 1;
        for (uint32_t d = 0; d < (GRID * GRID); d++) {
            uint32_t x0, y0, x1, y1;
            int32_t dx, dy;
            geo_lb_d2xy(d, GRID, &x0, &y0);
            geo_lb_d2xy((d + 1) % (GRID * GRID), GRID, &x1, &y1);
            geo_lb_direction((d + 1) % (GRID * GRID), GRID, &dx, &dy);
            int32_t vx = (int32_t)x1 - (int32_t)x0;
            int32_t vy = (int32_t)y1 - (int32_t)y0;
            if (vx > 1) vx = -1;
            if (vx < -1) vx = 1;
            if (vy > 1) vy = -1;
            if (vy < -1) vy = 1;
            if (vx != dx || vy != dy) ok = 0;
        }
        CHECK(17, "E4: direction(d+1) = displacement จริง d→d+1 (รู้ทางล่วงหน้าทุก step)", ok);
    }

    /* E5: สรุป bookmark — address → (rotation, cells, ทิศ) ครบในฟังก์ชันเดียว */
    {
        int32_t cells[4][2];
        uint32_t rot; int32_t dx, dy;
        geo_lb_from_hilbert(42, GRID, cells, &rot, &dx, &dy);
        printf("        ตัวอย่าง bookmark d=42 → rot=%u dir=(%+d,%+d) cells=[(%d,%d)(%d,%d)(%d,%d)(%d,%d)]\n",
               rot, dx, dy,
               cells[0][0], cells[0][1], cells[1][0], cells[1][1],
               cells[2][0], cells[2][1], cells[3][0], cells[3][1]);
    }
}

/* ── D. negative — เสาเข็มห้ามขยับ: scale ผิด → คนละ slot → mismatch ── */
static void test_negative(void) {
    printf("\nD. negative — address คือหลัก: scale ผิด → คนละที่ → mismatch\n");
    enum { CELL = 64, GRID = 16 };
    uint8_t container[256][CELL];
    uint8_t chunk[CELL];
    for (int i = 0; i < 256; i++) {
        for (int b = 0; b < CELL; b++)
            chunk[b] = (uint8_t)((i * 31 + b * 7 + 13) & 0xFF);
        memcpy(container[i], chunk, CELL);
    }
    /* chunk 5 อยู่ที่ from=5 → slot 5 · อ่านด้วย from=6 (scale ผิด) → slot 6 = data ของ chunk 6 */
    uint64_t wrong = rdh_addr(0, 6);
    uint32_t d = (uint32_t)(wrong % (GRID * GRID));
    uint32_t slot = geo_lb_slot(d, GRID);
    uint8_t expect5[CELL];
    for (int b = 0; b < CELL; b++)
        expect5[b] = (uint8_t)((5 * 31 + b * 7 + 13) & 0xFF);
    int mismatch = (slot != 5) && (memcmp(container[slot], expect5, CELL) != 0);
    CHECK(13, "D1: อ่านด้วย scale ผิด → slot ต่าง + data ไม่ตรง (จับได้)", mismatch);
    printf("        (from=6 → slot %u แต่ chunk 5 อยู่ slot 5 · data ต่างกันแน่)\n", slot);
}

int main(void) {
    printf("═══ test_geo_lblock — L-block summon + fit + RDH chain ═══\n");
    test_port();
    test_fit();
    test_chain();
    test_negative();
    test_bookmark_expand();
    printf("\n═══════════════════════════════════════\n");
    printf("RESULT: %d PASS / %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
}
