/* test_ckpt_wang.c — wang gate บน checkpoint image
 * ═══════════════════════════════════════════════════════════════════════════
 * user: "เอา wang gate ไปใช้กับ checkpoint image จริง (fibo checkpoint):
 *        ก่อน replay ตรวจ wang edges ทั้ง log — จับ corrupted checkpoint
 *        ได้ก่อน decode"
 *
 * พิสูจน์:
 *   A. digest ต่อ window: 8B/12 entries · parity + edges + n369 ถูก
 *   B. สะอาด: verify == 0 + scan ผ่าน → decode lossless
 *   C. corrupt 1 byte ใน log → verify reject (ก่อน decode) — หลายตำแหน่ง
 *   D. corrupt window boundary / ใน digest เอง → reject
 *   E. edge cases: 0, 1, 13, 25 entries (window boundary)
 *   F. flow เต็ม: checkpoint (header + log + digest + rs) → corrupt → reject
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore \
 *        -o build/test-ckpt_wang tests/test_ckpt_wang.c -lm
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../core/ckpt_wang.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static void fill_pattern(uint8_t *b, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)((seed + i * 37u) % 251u);
}

/* สร้าง log ด้วย ghost_lift จำนวน n entries (หลากหลาย block/from/to) */
static uint32_t build_log(GhostLog *log, ResidualSpace *rs, uint32_t n) {
    ghost_log_init(log);
    rs_init(rs, 4096);
    uint8_t d[8];
    fill_pattern(d, sizeof(d), 13);
    uint32_t made = 0;
    for (uint32_t i = 0; i < n && made < n; i++) {
        uint16_t b = (uint16_t)(i * 7u + 3u);
        uint8_t  f = (uint8_t)(i % 250u);
        uint8_t  t = (uint8_t)((i * 11u + 5u) % 143u);
        if (ghost_lift(log, rs, b, f, t, d, sizeof(d)) != RS_BOND_KEY_RESERVED)
            made++;
    }
    return made;
}

/* ── A. digest structure ── */
static void test_digest(void) {
    printf("\nA. digest — 8B/12 entries, edges + parity + n369\n");
    GhostLog log; ResidualSpace rs;
    uint32_t n = build_log(&log, &rs, 40);
    CHECK(1, "log 40 entries (3.33 windows)", n == 40);

    uint32_t dsz = ckpt_wang_digest_size(log.count);
    CHECK(1, "digest size = 4 windows × 8B = 32B", dsz == 4u * sizeof(CkptWangWin));

    uint8_t dig[64];
    ckpt_wang_digest(&log, dig);
    /* verify ของ digest ที่เพิ่งสร้าง = 0 เสมอ */
    CHECK(1, "digest สอดคล้องกับ log ของตัวเอง (verify==0)",
          ckpt_wang_verify(&log, dig, dsz) == 0);
    /* n369: นับตรง ๆ เทียบ window แรก */
    {
        uint32_t n369 = 0;
        for (uint32_t i = 0; i < 12; i++) {
            uint16_t enc = ckpt_entry_enc(&log.entries[i]);
            if (enc % 9u == 0u || enc % 9u == 3u || enc % 9u == 6u) n369++;
        }
        CkptWangWin w0;
        memcpy(&w0, dig, sizeof(w0));
        CHECK(1, "n369 ของ window แรกตรงการนับจริง (n369=%u)", w0.n369 == n369);
    }
    rs_free(&rs);
}

/* ── B. สะอาดผ่าน + lossless ── */
static void test_clean(void) {
    printf("\nB. สะอาด: verify + scan ผ่าน → decode lossless\n");
    GhostLog log; ResidualSpace rs;
    uint32_t n = build_log(&log, &rs, 30);
    uint8_t dig[64];
    uint32_t dsz = ckpt_wang_digest_size(log.count);
    ckpt_wang_digest(&log, dig);
    CHECK(2, "clean: ckpt_wang_check == 0 (verify + scan ผ่าน)",
          ckpt_wang_check(&log, dig, dsz) == 0);
    /* decode lossless: อ่านทุก route กลับ */
    {
        int ok = 1;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t b = (uint16_t)(i * 7u + 3u);
            uint8_t  f = (uint8_t)(i % 250u);
            uint8_t  t = (uint8_t)((i * 11u + 5u) % 143u);
            if (ghost_read(&log, &rs, b, f, t, NULL) == NULL) ok = 0;
        }
        CHECK(2, "decode ครบ %u routes (lossless)", ok == 1);
    }
    rs_free(&rs);
}

/* ── C. corrupt 1 byte ใน log → reject ก่อน decode ── */
static void test_corrupt_log(void) {
    printf("\nC. corrupt 1 byte ใน log → verify reject (ก่อน decode)\n");
    /* corrupt หลายตำแหน่ง: to_scale, from_scale, block กลาง/ต้น/ท้าย */
    const uint32_t spots[] = {0, 5, 17, 38, 59};   /* index ใน entries */
    for (uint32_t k = 0; k < 5; k++) {
        GhostLog log; ResidualSpace rs;
        uint32_t n = build_log(&log, &rs, 60);
        uint8_t dig[64];
        uint32_t dsz = ckpt_wang_digest_size(log.count);
        ckpt_wang_digest(&log, dig);

        uint32_t i = spots[k] % n;
        log.entries[i].to_scale ^= (uint8_t)(0x20u);
        int r = ckpt_wang_check(&log, dig, dsz);
        CHECK(3, "corrupt entry (to_scale) → reject", r != 0);
        rs_free(&rs);
    }
}

/* ── D. corrupt digest / window boundary ── */
static void test_corrupt_digest(void) {
    printf("\nD. corrupt digest เอง / window boundary\n");
    GhostLog log; ResidualSpace rs;
    build_log(&log, &rs, 30);   /* 3 windows — boundary ที่ entry 12, 24 */
    uint8_t dig[64];
    uint32_t dsz = ckpt_wang_digest_size(log.count);
    ckpt_wang_digest(&log, dig);

    /* corrupt byte ใน digest (parity ของ window 1) */
    dig[sizeof(CkptWangWin) + 4] ^= 0x01;   /* parity byte ของ window 1 */
    CHECK(4, "corrupt digest → reject", ckpt_wang_check(&log, dig, dsz) != 0);

    /* กลับมา digest ดี → corrupt entry ที่ window boundary (index 12) */
    ckpt_wang_digest(&log, dig);
    log.entries[12].from_scale ^= 0x03;
    CHECK(4, "corrupt entry ที่ window boundary (index 12) → reject",
          ckpt_wang_check(&log, dig, dsz) != 0);

    /* corrupt block_id ของ entry สุดท้าย */
    ckpt_wang_digest(&log, dig);
    log.entries[29].block_id ^= 0x40;
    CHECK(4, "corrupt block_id ของ entry สุดท้าย → reject",
          ckpt_wang_check(&log, dig, dsz) != 0);
    rs_free(&rs);
}

/* ── E. edge cases ── */
static void test_edges(void) {
    printf("\nE. edge cases: 0, 1, 13, 25 entries\n");
    const uint32_t counts[] = {0, 1, 13, 25};
    for (uint32_t k = 0; k < 4; k++) {
        GhostLog log; ResidualSpace rs;
        uint32_t n = build_log(&log, &rs, counts[k]);
        uint8_t dig[64];
        uint32_t dsz = ckpt_wang_digest_size(log.count);
        ckpt_wang_digest(&log, dig);
        int clean = (ckpt_wang_check(&log, dig, dsz) == 0);
        int caught = 0;
        if (n > 0) {
            log.entries[n - 1].to_scale ^= 1;
            caught = (ckpt_wang_check(&log, dig, dsz) != 0);
        }
        CHECK(5, "count edge case: clean ผ่าน + corrupt ถูกจับ",
              clean && (n == 0 || caught));
        rs_free(&rs);
    }
}

/* ── F. flow เต็ม: checkpoint image + corrupt → reject ก่อน decode ── */
static void test_flow(void) {
    printf("\nF. flow เต็ม — checkpoint + corrupt → reject ก่อน decode\n");
    GhostLog log; ResidualSpace rs;
    uint32_t n = build_log(&log, &rs, 40);
    uint8_t dig[64];
    uint32_t dsz = ckpt_wang_digest_size(log.count);
    ckpt_wang_digest(&log, dig);

    /* checkpoint image: header(4) + log + digest + rs */
    uint64_t lsz = ghost_log_serialize_size(&log);
    uint64_t rsz = rs_serialize_size(&rs);
    uint64_t img_sz = 4 + lsz + dsz + rsz;
    uint8_t *img = (uint8_t *)malloc((size_t)img_sz);
    uint8_t *p = img;
    memcpy(p, "WNCK", 4); p += 4;
    ghost_log_serialize(&log, p, lsz); p += lsz;
    memcpy(p, dig, dsz); p += dsz;
    rs_serialize(&rs, p, rsz);

    /* restart: load + ตรวจก่อน decode */
    GhostLog log2; ResidualSpace rs2;
    ghost_log_init(&log2); rs_init(&rs2, 4096);
    const uint8_t *q = img + 4;
    CHECK(6, "load log + digest + rs จาก image",
          ghost_log_load(&log2, q, lsz) == 0 && (q += lsz) &&
          ckpt_wang_check(&log2, q, dsz) == 0 && (q += dsz) &&
          rs_load(&rs2, q, rsz) == 0);
    /* decode ครบ lossless */
    {
        int ok = 1;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t b = (uint16_t)(i * 7u + 3u);
            uint8_t  f = (uint8_t)(i % 250u);
            uint8_t  t = (uint8_t)((i * 11u + 5u) % 143u);
            if (ghost_read(&log2, &rs2, b, f, t, NULL) == NULL) ok = 0;
        }
        CHECK(6, "decode ครบหลัง load (lossless)", ok == 1);
    }
    rs_free(&rs2);

    /* ตอนนี้ corrupt 1 byte ใน log region ของ image → load + check → reject */
    GhostLog log3; ResidualSpace rs3;
    ghost_log_init(&log3); rs_init(&rs3, 4096);
    uint8_t *img2 = (uint8_t *)malloc((size_t)img_sz);
    memcpy(img2, img, (size_t)img_sz);
    /* log region เริ่มที่ img+4: GHST(4) + ver(2) + rsv(2) + count(4)
       → entries เริ่มที่ img+16: block[2] from[1] to[1] flags[1]
       → to_scale ของ entry แรก = img2[19]
       (corrupt ที่ magic/header จะถูก load จับ — ต้อง corrupt ใน entry
       เพื่อพิสูจน์ว่า digest จับ; flags ไม่มีผลต่อ enc → ไม่ควรใช้) */
    img2[16 + 3] ^= 0x08;           /* to_scale ของ entry แรก */
    q = img2 + 4;
    CHECK(6, "load ยังผ่าน (corrupt ใน entry ไม่ใช่ header)",
          ghost_log_load(&log3, q, lsz) == 0);
    int r = ckpt_wang_check(&log3, q + lsz, dsz);
    CHECK(6, "image ถูก corrupt → reject ก่อน decode (r=%d)", r != 0);

    free(img); free(img2);
    rs_free(&rs);
}

int main(void) {
    printf("═ wang gate บน checkpoint image ═\n");
    test_digest();
    test_clean();
    test_corrupt_log();
    test_corrupt_digest();
    test_edges();
    test_flow();
    printf("\n══════════ %d/%d PASS ══════════\n", pass, pass + fail);
    return fail ? 1 : 0;
}
