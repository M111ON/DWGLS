/* test_tied_dedup.c — §15.75: registry {tensor_id → home} — byte-identical
 * tensors freeze ครั้งเดียว (MAP not COMPRESS)
 * ═══════════════════════════════════════════════════════════════════════════
 *   T1  scan: FNV candidate + memcmp verify → home mapping ถูกต้อง (t2→t1,
 *      t5→t0) · same-size ต่างเนื้อหาไม่ merge (t4) · ว่าง/ไม่มี data ข้าม
 *   T2  scan หลังแก้ 1 byte → ไม่ merge อีก (memcmp verify ทำงาน)
 *   T3  place dedup ON + verify — ทุก tensor lossless (dup ผ่าน route → home)
 *   T4  place dedup OFF (baseline) + verify — lossless เช่นกัน
 *   T5  dup ไม่ freeze: frozen(OFF) − frozen(ON) == dup bytes (800000) —
 *      freeze ครั้งเดียวจริง (physical)
 *   T6  lift count: ON 9 chunks / OFF 13 chunks (dup ข้าม placement)
 *   T7  route = address: thaw bond ของ home chunk → ได้ bytes ของ dup ด้วย
 *      (อ่าน dup = resolve route → home bond เดียว)
 *
 * BUILD: make test-test_tied_dedup
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/tied_dedup.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* deterministic pseudo-random — byte ครบทุกค่า */
static uint64_t rng_state = UINT64_C(0x71A11EEDBEEF0001);
static uint8_t next_byte(void) {
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint8_t)(rng_state * UINT64_C(0x2545F4914F6CDD1D) >> 56);
}

static void fill(uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = next_byte(); }

/* oracle: นับ lifted chunks ตามกฎ CAP_RULE_* (อิสระจาก tied_place — ทดสอบ plumbing) */
static uint64_t oracle_lifts(uint32_t size) {
    uint32_t k_max = ght_envelope_depth(CAP_RULE_GATE);
    uint64_t nchunks = (size + CAP_RULE_CHUNK - 1) / CAP_RULE_CHUNK;
    uint64_t lifts = 0;
    for (uint64_t r = 0; r < nchunks; r++) {
        uint8_t w = (uint8_t)(((uint64_t)CAP_RULE_STRIDE * r + CAP_RULE_OFFSET) % 144u);
        if (w > k_max) lifts++;
    }
    return lifts;
}

int main(void) {
    printf("═══ TIED-EMBEDDING DEDUP → CHAIN (§15.75) ═══\n");

    enum { N = 8 };
    uint32_t sz[N];
    uint8_t *buf[N];
    const uint8_t *data[N];
    int32_t home[N];
    memset(home, 0, sizeof(home));

    sz[0] = 100000;                                   /* 1 chunk — home A */
    sz[1] = 700000;                                   /* 3 chunks — home B */
    sz[2] = 700000;                                   /* dup ของ t1 (tied) */
    sz[3] = 300000;                                   /* 2 chunks */
    sz[4] = 700000;                                   /* size เดียวกับ t1 แต่ต่างเนื้อหา */
    sz[5] = 100000;                                   /* dup ของ t0 */
    sz[6] = 0;                                        /* ว่าง — ข้าม */
    sz[7] = 5;                                        /* ไม่มี data — ข้าม */

    for (int i = 0; i <= 5; i++) buf[i] = (uint8_t *)malloc(sz[i]);
    fill(buf[0], sz[0]);
    fill(buf[1], sz[1]);
    buf[2] = (uint8_t *)malloc(sz[2]); memcpy(buf[2], buf[1], sz[2]);   /* tied */
    fill(buf[3], sz[3]);
    fill(buf[4], sz[4]);                              /* ต่างเนื้อหาจริง */
    buf[5] = (uint8_t *)malloc(sz[5]); memcpy(buf[5], buf[0], sz[5]);   /* tied */
    buf[6] = NULL; buf[7] = NULL;

    for (int i = 0; i < N; i++) data[i] = buf[i];

    /* ── T1: scan → home mapping ── */
    uint64_t dup = tied_dedup_scan(data, sz, N, home);
    CHECK(1, "t2 → home t1 (tied pair 700KB)",
          home[2] == 1 && home[1] == 1);
    CHECK(1, "t5 → home t0 (tied pair 100KB)",
          home[5] == 0 && home[0] == 0);
    CHECK(1, "t4 (same size, ต่างเนื้อหา) ไม่ merge", home[4] == 4);
    CHECK(1, "t3 เป็น home ของตัวเอง", home[3] == 3);
    CHECK(1, "t6 (size 0) / t7 (no data) ข้าม", home[6] == -1 && home[7] == -1);
    CHECK(1, "dup bytes == 800000 (700K+100K)", dup == 800000u);

    /* ── T2: แก้ 1 byte → memcmp verify จับได้ ไม่ merge ── */
    buf[5][17] ^= 0xFF;
    int32_t home2[N];
    uint64_t dup2 = tied_dedup_scan(data, sz, N, home2);
    CHECK(2, "แก้ 1 byte → t5 ไม่ merge (home ตนเอง)", home2[5] == 5);
    CHECK(2, "dup ลดเหลือ 700000", dup2 == 700000u);
    buf[5][17] ^= 0xFF;   /* กลับค่าเดิม */

    /* ── rs sizing: ไม่ evict (load ≤ 0.5) ── */
    uint64_t need = 0;
    for (int i = 0; i <= 5; i++) need += (sz[i] + RS_MAX_DATA_SIZE - 1) / RS_MAX_DATA_SIZE;
    uint32_t rs_cap = 64;
    while (rs_cap < need * 2) rs_cap <<= 1;
    uint32_t max_t = 0;
    for (int i = 0; i <= 5; i++) if (sz[i] > max_t) max_t = sz[i];
    uint8_t *scratch = (uint8_t *)malloc(max_t ? max_t : 1);

    uint64_t base_gid[N];
    TiedChainStats st_on, st_off;

    /* ── T3: place dedup ON + verify ── */
    ResidualSpace rs;
    rs_init(&rs, rs_cap);
    int place_ok = tied_place(&rs, data, sz, N, home, base_gid, &st_on);
    int verify_ok = (tied_verify(&rs, data, sz, N, home, base_gid, scratch, max_t) == 0);
    CHECK(3, "place (dedup ON) ไม่ fail", place_ok == 0);
    CHECK(3, "verify (dedup ON) — ทุก tensor lossless รวม dup ผ่าน route",
          verify_ok == 1);
    CHECK(3, "dup ไม่กิน field (field เหลือ 0 — ทุก chunk lift)", st_on.field_slots == 0);

    /* ── T7: route = address — thaw bond ของ home → ได้ bytes ของ dup ด้วย ── */
    {
        uint32_t k_max = ght_envelope_depth(CAP_RULE_GATE);
        uint8_t w = (uint8_t)(((uint64_t)CAP_RULE_STRIDE * 0 + CAP_RULE_OFFSET) % 144u);
        uint32_t got = 0;
        PoglsPiece p = ghost_piece((uint16_t)base_gid[1], 0, w);   /* chunk 0 ของ t1 */
        const uint8_t *home_chunk = (const uint8_t *)rs_thaw(&rs, pogls_bond_key(&p), &got);
        int same = 0;
        if (home_chunk && got <= sz[1] && w > k_max) {
            uint32_t cl = sz[1]; if (cl > CAP_RULE_CHUNK) cl = CAP_RULE_CHUNK;
            uint32_t sl = cl; if (sl > RS_MAX_DATA_SIZE) sl = RS_MAX_DATA_SIZE;
            same = (got == sl && memcmp(home_chunk, buf[1], sl) == 0 &&
                    memcmp(home_chunk, buf[2], sl) == 0);   /* dup ต้องได้ bytes เดียว */
        }
        CHECK(7, "home bond chunk 0 == t1 bytes == t2 bytes (route ชี้ home เดียว)", same == 1);
    }
    rs_free(&rs);

    /* ── T4: place dedup OFF (baseline) + verify ── */
    int32_t home_off[N];
    for (int i = 0; i < N; i++) home_off[i] = (data[i] && sz[i] > 0) ? i : -1;
    rs_init(&rs, rs_cap);
    place_ok = tied_place(&rs, data, sz, N, home_off, base_gid, &st_off);
    verify_ok = (tied_verify(&rs, data, sz, N, home_off, base_gid, scratch, max_t) == 0);
    CHECK(4, "place (dedup OFF) ไม่ fail", place_ok == 0);
    CHECK(4, "verify (dedup OFF) — lossless", verify_ok == 1);
    rs_free(&rs);

    /* ── T5: dup ไม่ freeze — physical delta == dup bytes ── */
    CHECK(5, "frozen(OFF) − frozen(ON) == 800000 (dup ไม่ freeze — freeze ครั้งเดียว)",
          st_off.frozen_bytes - st_on.frozen_bytes == 800000u);
    CHECK(5, "frozen(ON) == home tensors เท่านั้น (1,800,000)",
          st_on.frozen_bytes == 1800000u);

    /* ── T6: lift count — dup ข้าม placement ── */
    uint64_t lift_on  = oracle_lifts(sz[0]) + oracle_lifts(sz[1]) + oracle_lifts(sz[3]) + oracle_lifts(sz[4]);
    uint64_t lift_off = lift_on + oracle_lifts(sz[2]) + oracle_lifts(sz[5]);
    CHECK(6, "lifts ON == 9 (home เท่านั้น)", st_on.lifts == lift_on && lift_on == 9);
    CHECK(6, "lifts OFF == 13 (ทุก tensor)", st_off.lifts == lift_off && lift_off == 13);

    /* cleanup */
    for (int i = 0; i < N; i++) if (buf[i]) free(buf[i]);
    free(scratch);

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass, fail);
    return fail ? 1 : 0;
}
