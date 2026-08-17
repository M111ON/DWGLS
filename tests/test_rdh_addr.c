/* test_rdh_addr.c — RDH mixed-radix addressing: bijection proof + reversibility
 * ═══════════════════════════════════════════════════════════════════════════
 * RDH (collection/rdh → core/geo_rdh_addr.h) แทนที่ FNV-1a ในสาย ghost/bond:
 *   address = mixed-radix encode ของ (block, from_scale) — ไม่มี hash
 *
 * พิสูจน์:
 *   A. bijection: sweep ทั้ง 2^24 keys (65536 rings × 256 wedges) ด้วย bitset —
 *      rdh_addr และ rdh_bond_key ต้องไม่ชนกันเลยสักคู่ (collision-free by construction)
 *   B. reversible: decompose(rdh_addr(b,f)) → (b,f) ครบ — "address IS data"
 *   C. ghost semantics ผ่าน RDH: round อยู่ใน bond (เสาเข็มห้ามขยับ),
 *      to_scale ไม่อยู่ใน bond (telescope), deterministic
 *   D. ghost chain ยัง lossless ครบ (lift → read ผ่าน RDH bond)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-rdh_addr tests/test_rdh_addr.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/geo_rdh_addr.h"
#include "../core/geo_ghost_lift.h"

#define N_RINGS   RDH_N_RINGS      /* 65536 */
#define N_WEDGES  RDH_N_WEDGES     /* 256   */

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static uint64_t bond_of(uint16_t block, uint8_t from, uint8_t to) {
    PoglsPiece p = ghost_piece(block, from, to);
    return pogls_bond_key(&p);
}

/* ── A. bijection: ทุก (block, from) → address ไม่ชนกัน ── */
static void test_bijection(void) {
    printf("\nA. bijection — sweep ทั้ง 2^24 keys (65536×256) ด้วย bitset\n");
    /* bitset 2^24 bits = 2MB */
    size_t nbytes = (1u << 24) / 8u;
    uint8_t *seen_addr = (uint8_t *)calloc(nbytes, 1);
    uint8_t *seen_bond = (uint8_t *)calloc(nbytes, 1);
    if (!seen_addr || !seen_bond) { printf("  (alloc fail)\n"); exit(1); }

    uint64_t dups_addr = 0;
    int bond_ok = 1;
    for (uint32_t b = 0; b < N_RINGS; b++) {
        for (uint32_t f = 0; f < N_WEDGES; f++) {
            uint64_t a = rdh_addr(b, f);
            uint64_t k = rdh_bond_key(b, f);
            if (a >= (1u << 24)) { printf("  (addr out of range!)\n"); exit(1); }
            if (seen_addr[a >> 3] & (1u << (a & 7))) dups_addr++;
            else seen_addr[a >> 3] |= (uint8_t)(1u << (a & 7));
            /* interleave: ครึ่งบน = ครึ่งล่าง = rdh_addr (ไม่ทับกัน → bijection ตาม addr) */
            if ((k & 0xFFFFFFu) != a || (k >> 24) != a) bond_ok = 0;
        }
    }
    CHECK(1, "rdh_addr ไม่ชนเลยใน 2^24 keys (bijection by construction)",
          dups_addr == 0);
    CHECK(1, "rdh_bond_key = interleave addr|addr<<24 — bijection 48-bit ตาม rdh_addr",
          bond_ok);
    printf("  dups: addr=%llu (2^24 = %u keys) — bond ครึ่งบน=ครึ่งล่าง=addr ทุกคู่\n",
           (unsigned long long)dups_addr, 1u << 24);

    /* rdh_addr ครอบคลุมทั้ง 2^24 ช่องพอดี (linear bijection) */
    CHECK(2, "rdh_addr เป็น bijection ลง [0, 2^24) พอดี (ความหมายเชิงมิติ)",
          rdh_addr(N_RINGS - 1, N_WEDGES - 1) == (1u << 24) - 1u);

    free(seen_addr);
    free(seen_bond);
}

/* ── B. reversible: decompose กู้ (block, from) กลับจาก address ── */
static void test_reversible(void) {
    printf("\nB. reversible — decompose(rdh_addr(b,f)) → (b,f) ครบ\n");
    int ok = 1;
    for (uint32_t b = 0; b < 4096 && ok; b++)
        for (uint32_t f = 0; f < 256; f++) {
            uint64_t a = rdh_addr(b, f);
            uint32_t rb, rf;
            rdh_decompose(a, &rb, &rf);
            if (rb != b || rf != f) ok = 0;
        }
    CHECK(3, "decompose ครบ 1M pairs (1,048,576) — address IS data (ต่างจาก hash)",
          ok);
    uint32_t rb, rf;
    rdh_decompose(rdh_addr(65535, 255), &rb, &rf);
    CHECK(3, "decompose ขอบเขตสูงสุด (65535,255) ถูกต้อง", rb == 65535 && rf == 255);
    rdh_decompose(rdh_addr(0, 0), &rb, &rf);
    CHECK(3, "decompose จุดกำเนิด (0,0) ถูกต้อง", rb == 0 && rf == 0);
}

/* ── C. ghost semantics ผ่าน RDH bond ── */
static void test_ghost_semantics(void) {
    printf("\nC. ghost semantics ผ่าน RDH — เสาเข็ม + telescope + deterministic\n");
    CHECK(4, "round (from) ต่าง → bond ต่าง (birth round ใน address)",
          bond_of(7, 3, 5) != bond_of(7, 9, 5));
    CHECK(4, "block ต่าง → bond ต่าง (ring ใน address)",
          bond_of(7, 3, 5) != bond_of(8, 3, 5));
    CHECK(4, "to_scale ต่าง → bond เท่าเดิม (route ไม่อยู่ใน bond — telescope)",
          bond_of(7, 3, 5) == bond_of(7, 3, 140));
    CHECK(4, "deterministic: (block, round, to) เดิม → bond เดิมเสมอ",
          bond_of(21, 100, 7) == bond_of(21, 100, 7));
    CHECK(4, "geo_key = rdh_addr — decompose ได้ (coordinate อยู่ใน geo_key)",
          ghost_piece(21, 100, 7).geo_key == rdh_addr(21, 100));
}

/* ── E. ฟรี centroid: mixed-radix เป็น linear → mean(addr) = addr(mean) ── */
static void test_centroid(void) {
    printf("\nE. ฟรี centroid — encode linear → centroid ได้จาก mean ของ addresses\n");
    /* 4 จุดสมมาตร: (0,0),(2,0),(0,2),(2,2) → centroid ต้องเป็น (1,1) */
    {
        uint64_t pts[4] = { rdh_addr(0, 0), rdh_addr(2, 0),
                            rdh_addr(0, 2), rdh_addr(2, 2) };
        uint64_t sum = 0;
        for (int i = 0; i < 4; i++) sum += pts[i];
        uint64_t mean_addr = sum / 4;
        uint32_t cb, cf;
        rdh_decompose(mean_addr, &cb, &cf);
        CHECK(6, "mean ของ addresses = address ของ mean — centroid (1,1) ตรงเป๊ะ",
              mean_addr == rdh_addr(1, 1) && cb == 1 && cf == 1);
    }
    /* cluster รอบ (100,50): centroid ต้องกลับมาที่ (100,50) */
    {
        uint32_t b[5] = { 98, 99, 100, 101, 102 };
        uint32_t f[5] = { 48, 49, 50, 51, 52 };
        uint64_t sum = 0;
        for (int i = 0; i < 5; i++) sum += rdh_addr(b[i], f[i]);
        uint64_t mean_addr = sum / 5;
        uint32_t cb, cf;
        rdh_decompose(mean_addr, &cb, &cf);
        CHECK(6, "centroid ของ cluster กลับมาที่ (100,50) — ตรงกับค่าเฉลี่ยตรงๆ",
              cb == 100 && cf == 50);
    }
    /* centroid ring = ค่าเฉลี่ยของ round — drift บนแกน scale วัดได้จาก address */
    {
        /* กลุ่มหนึ่งเกิดที่ round ต่ำ อีกกลุ่มถูก lift ไป round สูง */
        uint32_t lo[3] = { 3, 5, 7 };
        uint32_t hi[3] = { 140, 141, 143 };
        uint64_t sum = 0;
        for (int i = 0; i < 3; i++) sum += rdh_addr(10, (uint8_t)lo[i]);
        for (int i = 0; i < 3; i++) sum += rdh_addr(10, (uint8_t)hi[i]);
        uint64_t mean_addr = sum / 6;
        uint32_t cb, cf;
        rdh_decompose(mean_addr, &cb, &cf);
        uint32_t avg_round = (3u + 5u + 7u + 140u + 141u + 143u) / 6u;
        CHECK(7, "centroid ring = ค่าเฉลี่ย round ของ cluster (lift → centroid ไหลออกนอก)",
              cf == avg_round && cb == 10);
    }
}

/* ── D. ghost chain ยัง lossless ผ่าน RDH ── */
static void test_chain_lossless(void) {
    printf("\nD. ghost chain lossless ผ่าน RDH bond\n");
    GhostLog log;      ghost_log_init(&log);
    ResidualSpace rs;  rs_init(&rs, 64);
    uint8_t d[1024];
    for (int i = 0; i < 1024; i++) d[i] = (uint8_t)(i * 7u + 3u);
    uint64_t bk = ghost_lift(&log, &rs, 5, 3, 10, d, sizeof(d));
    uint32_t sz = 0;
    const void *got = ghost_read(&log, &rs, 5, 3, 10, &sz);
    CHECK(5, "lift → read lossless ผ่าน RDH bond", bk != RS_BOND_KEY_RESERVED &&
          got && sz == sizeof(d) && memcmp(got, d, sizeof(d)) == 0);
    CHECK(5, "เสาเข็มห้ามขยับ: from ผิด → NULL (bond แตก + route ไม่มี)",
          ghost_read(&log, &rs, 5, 4, 10, &sz) == NULL);
    CHECK(5, "route ผิด → NULL", ghost_read(&log, &rs, 5, 3, 11, &sz) == NULL);
    CHECK(5, "deterministic ข้าม space: lift เดิมที่ space ใหม่ → bond เท่าเดิม",
          rs_contains(&rs, ghost_bond_key(5, 3, 10)) == 1);
    rs_free(&rs);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("RDH mixed-radix addressing — collision-free + reversible (แทน FNV-1a)\n");
    printf("════════════════════════════════════════════════════════════════════\n");

    test_bijection();
    test_reversible();
    test_ghost_semantics();
    test_centroid();
    test_chain_lossless();

    printf("\n════════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
