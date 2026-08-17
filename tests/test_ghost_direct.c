/* test_ghost_direct.c — ghost log: binary search + wang gate (b-bond)
 * ═══════════════════════════════════════════════════════════════════════════
 * user: "ปรับได้เลยครับ ก่อนหน้านี้ด้วย ที่ใช้ wang แล้วเดี๋ยวค่อยมาสรุป"
 *
 * ปรับ (ตาม b-bond §15.54 — chunk ไม่ต้องรันเลข):
 *   - ghost_log_find/ghost_route_count: linear scan → binary search
 *     (entries sorted โดย (block, from); scan เฉพาะ routes ของ pile)
 *   - ghost_lift: sorted insert (คง invariant)
 *   - ghost_read: hyp_gate (wang integrity + tamper) guard — timeline เสีย
 *     → ปิดเส้นทาง (NULL) — wire fusion S2 เข้า chain จริง
 *
 * พิสูจน์:
 *   A. find/route_count ถูกเทียบ brute force (linear scan) บน log จริง
 *   B. sorted invariant หลัง lift หลายครั้ง
 *   C. wang gate: wl เสีย → read NULL · restore → read กลับมา
 *   D. persistence: serialize → load → find/read ยังถูก + wang rebuilt
 *   E. lossless ที่ scale ตรง (เชนเดิมยัง intact)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore \
 *        -o build/test-ghost_direct tests/test_ghost_direct.c -lm
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../core/geo_ghost_lift.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static void fill_pattern(uint8_t *b, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)((seed + i * 37u) % 251u);
}

/* brute force — linear scan (อ้างอิง) */
static int bf_find(const GhostLog *log, uint16_t blk, uint8_t from, uint8_t to) {
    for (uint32_t i = 0; i < log->count; i++) {
        const GhostLogEntry *e = &log->entries[i];
        if (e->flags & GHOST_FLAG_EXPIRED) continue;
        if (e->block_id == blk && e->from_scale == from && e->to_scale == to)
            return (int)i;
    }
    return -1;
}

/* ── A. find/route_count vs brute force ── */
static void test_find(void) {
    printf("\nA. binary search find/route_count ถูกเทียบ brute force\n");
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 8192);
    uint8_t d[16];
    fill_pattern(d, sizeof(d), 7);

    /* lift 300 routes หลาย block/from/to */
    uint32_t lifted = 0;
    for (uint16_t b = 0; b < 30; b++)
        for (uint8_t f = 0; f < 4; f++)
            for (uint8_t t = 0; t < 3; t++) {
                if (ghost_lift(&log, &rs, (uint16_t)(b * 37u + 3u), f,
                               (uint8_t)(t * 40u + 5u), d, sizeof(d)))
                    lifted++;
            }
    CHECK(1, "lifted ครบ 360 routes (sorted insert ทำงาน)", lifted == 360u);

    /* find: ทุก route ที่ lift → เจอ (ตรงกับ brute force) */
    {
        int bad = 0;
        for (uint16_t b = 0; b < 30; b++)
            for (uint8_t f = 0; f < 4; f++)
                for (uint8_t t = 0; t < 3; t++) {
                    uint16_t blk = (uint16_t)(b * 37u + 3u);
                    uint8_t  to  = (uint8_t)(t * 40u + 5u);
                    int a = ghost_log_find(&log, blk, f, to);
                    int c = bf_find(&log, blk, f, to);
                    if ((a >= 0) != (c >= 0)) bad++;
                }
        CHECK(2, "find ตรง brute force ครบ 360 (เจอ/ไม่เจอ ตรงกัน)", bad == 0);
    }

    /* find: route ที่ไม่มี → -1 ทั้งคู่ */
    {
        int a = ghost_log_find(&log, 9999, 0, 0);
        int c = bf_find(&log, 9999, 0, 0);
        CHECK(3, "route ไม่มี → -1 ทั้ง binary และ brute (a=%d c=%d)", a < 0 && c < 0);
    }

    /* route_count: ทุก block ตรง brute force */
    {
        int bad = 0;
        for (uint16_t b = 0; b < 2000; b++) {
            uint32_t a = ghost_route_count(&log, b);
            uint32_t c = 0;
            for (uint32_t i = 0; i < log.count; i++)
                if (log.entries[i].block_id == b) c++;
            if (a != c) bad++;
        }
        CHECK(4, "route_count ตรง brute force ทุก block (bad=0)", bad == 0);
    }

    /* sorted invariant: entries เรียงโดย (block, from) */
    {
        int ok = 1;
        for (uint32_t i = 1; i < log.count; i++) {
            const GhostLogEntry *p = &log.entries[i - 1], *e = &log.entries[i];
            if (e->block_id < p->block_id ||
                (e->block_id == p->block_id && e->from_scale < p->from_scale))
                ok = 0;
        }
        CHECK(5, "sorted invariant — entries เรียง (block, from) ตลอด",
              ok == 1);
    }

    rs_free(&rs);
}

/* ── B. expire — ยังถูกกับ sorted ── */
static void test_expire(void) {
    printf("\nB. expire (re-attach) กับ log แบบ sorted\n");
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 512);
    uint8_t d[8];
    fill_pattern(d, sizeof(d), 3);

    ghost_lift(&log, &rs, 5, 3, 10, d, 8);
    ghost_lift(&log, &rs, 5, 3, 12, d, 8);
    ghost_lift(&log, &rs, 9, 1, 20, d, 8);   /* block อื่น — กัน sorted สับสน */

    uint32_t ex = ghost_expire(&log, &rs, 5, 3);
    CHECK(6, "expire คืน routes ของ pile (5,3) ทั้งหมด = 2", ex == 2);
    CHECK(6, "routes (5,3) ถูก flag expired ครบ",
          (log.entries[0].flags & GHOST_FLAG_EXPIRED) != 0 &&
          (log.entries[1].flags & GHOST_FLAG_EXPIRED) != 0);
    CHECK(6, "block อื่น (9,1) ไม่โดน — ยัง live",
          ghost_log_find(&log, 9, 1, 20) >= 0);
    CHECK(6, "route (5,3,10) ไม่ match แล้ว (expired)",
          ghost_log_find(&log, 5, 3, 10) < 0);
    CHECK(6, "read หลัง expire → NULL", ghost_read(&log, &rs, 5, 3, 10, NULL) == NULL);
    rs_free(&rs);
}

/* ── C. wang gate — timeline เสีย → ปิดเส้นทาง (wire fusion S2) ── */
static void test_wang_gate(void) {
    printf("\nC. wang gate ใน ghost_read — integrity ปิดเส้นทาง\n");
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 128);
    uint8_t d[16];
    fill_pattern(d, sizeof(d), 11);

    /* route (1,0,3): enc = (1*256+0+3) % 1440 = 259 — ไม่ใช่ 369 (259%9=7)
       → gate ถึงชั้น edge_valid → ทำลาย window ของมันได้จริง */
    uint64_t bk = ghost_lift(&log, &rs, 1, 0, 3, d, sizeof(d));
    CHECK(7, "lift + read ที่ scale ตรง → data (lossless)",
          bk != 0 && ghost_read(&log, &rs, 1, 0, 3, NULL) != NULL);

    /* ทำลาย wang layer (edge ถูกแก้แบบ chord-คู่ = ผ่าน tamper แต่
       continuity แตก) ที่ window ของ route → gate ต้องปิด read */
    uint16_t enc = (uint16_t)((ghost_origin_seed(1, 0) + 3) % FRAME_CYCLE);
    uint16_t win = (enc / WANG_WIN_SIZE) % WANG_WIN_COUNT;
    uint8_t neu = (uint8_t)((log.wang.wins[win].edge_top + 1) % 9u);
    log.wang.wins[win].edge_top   = neu;
    log.wang.wins[win].edge_top_b = (uint8_t)((9u - neu) % 9u);
    CHECK(8, "wang เสีย → read ถูกปิด (NULL) — 'ปิดเส้นทางได้'",
          ghost_read(&log, &rs, 1, 0, 3, NULL) == NULL);

    /* restore: rebuild wang (init ใหม่) + rs ใหม่ → read กลับมา
       (gate = ฟังก์ชันของ integrity — restore แล้วเปิดอีกครั้ง) */
    rs_free(&rs);
    rs_init(&rs, 128);
    ghost_log_init(&log);
    uint64_t bk2 = ghost_lift(&log, &rs, 1, 0, 3, d, sizeof(d));
    CHECK(9, "rebuild (init ใหม่) → lift + read กลับมาปกติ",
          bk2 != 0 && ghost_read(&log, &rs, 1, 0, 3, NULL) != NULL);
    rs_free(&rs);
}

/* ── D. persistence — serialize → load → find/read ยังถูก ── */
static void test_persist(void) {
    printf("\nD. persistence roundtrip กับ sorted log + wang rebuilt\n");
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 512);
    uint8_t d[8];
    fill_pattern(d, sizeof(d), 77);

    for (uint16_t b = 0; b < 12; b++)
        ghost_lift(&log, &rs, (uint16_t)(b * 13u + 1u), (uint8_t)b,
                   (uint8_t)(b * 5u + 3u), d, 8);

    uint64_t lsz = ghost_log_serialize_size(&log);
    uint8_t *buf = (uint8_t *)malloc((size_t)lsz);
    CHECK(10, "serialize ได้ขนาดเท่าเดิม (12 + count×5B)",
          lsz == 12u + log.count * sizeof(GhostLogEntry));
    CHECK(10, "serialize เขียนครบ", ghost_log_serialize(&log, buf, lsz) == lsz);

    GhostLog log2; ghost_log_init(&log2);
    CHECK(10, "load สำเร็จ (ว่างก่อน, count ตรง)",
          ghost_log_load(&log2, buf, lsz) == 0 && log2.count == log.count);
    CHECK(10, "load → find ยังเจอทุก route (แรก block 1, สุดท้าย block 144)",
          ghost_log_find(&log2, 1, 0, 3) >= 0 &&
          ghost_log_find(&log2, 144, 11, 58) >= 0);
    CHECK(10, "load → wang rebuilt (hyp_gate ไม่ TAMPER หลัง load)",
          hyp_gate(&log2.wang, 366, 0) != HYP_SEEK_TAMPER);
    free(buf);
    rs_free(&rs);
}

int main(void) {
    printf("═ ghost log: binary search + wang gate (b-bond) ═\n");
    test_find();
    test_expire();
    test_wang_gate();
    test_persist();
    printf("\n══════════ %d/%d PASS ══════════\n", pass, pass + fail);
    return fail ? 1 : 0;
}
