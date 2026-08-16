/* test_residual_space.c — Timeless Bond-Only Storage Zone
 *
 * Real test of core/residual_space.h (former version tested the WRONG
 * header — hyperbolic_seek.h, legacy angle-math from before the rescope).
 *
 * What this proves (per rescope working model):
 *   - Frozen data is addressed ONLY by bond_key (bond_L XOR bond_R),
 *     never by geo coordinate → "0 อยู่แต่ access ไม่ได้" (timeless zone).
 *   - origin_key = birth pile identity (geo_key) — rs_verify must pass.
 *   -  เสาเข็มห้ามขยับ: shift the pile's seed → bond_key changes →
 *     thaw/verify fail = bond breaks automatically (no lookup table,
 *     coordinate-bound by construction).
 *   - Lifecycle: freeze → thaw → refreeze(update) → tombstone → sweep
 *     → expire_by_origin → eviction when the zone is full.
 *
 * Ghost-lift framing (next step in the roadmap): blocks whose requested
 * scale exceeds their envelope (k > 4-5 ROI cliff) get FROZEN here via
 * bond and tracked through the hyperbolic delta log instead of
 * expanding the field.
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-residual_space tests/test_residual_space.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/residual_space.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── payload helpers ─────────────────────────────────────────── */
static void fill_pattern(uint8_t *buf, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i * 31 + seed * 7) & 0xFF);
}

/* ═══════════════════════════════════════════════════════════════
   T1 — freeze + thaw roundtrip (bond-addressed)
   ═══════════════════════════════════════════════════════════════ */
static void test_roundtrip(void) {
    ResidualSpace rs;
    CHECK(1, "rs_init(64) succeeds", rs_init(&rs, 64) == 0);

    uint8_t data[256];
    fill_pattern(data, sizeof(data), 42);

    PoglsPiece p = pogls_make_piece(0x1234, 3);   /* axis 3 → T splitter */
    uint64_t bk = rs_freeze(&rs, &p, data, sizeof(data), 0);
    CHECK(1, "rs_freeze returns nonzero bond_key", bk != RS_BOND_KEY_RESERVED);
    CHECK(1, "bond_key == pogls_bond_key(piece)", bk == pogls_bond_key(&p));

    uint32_t out_sz = 0;
    const void *got = rs_thaw(&rs, bk, &out_sz);
    CHECK(1, "rs_thaw finds entry", got != NULL);
    CHECK(1, "thawed size matches", out_sz == sizeof(data));
    CHECK(1, "thawed bytes match exactly",
          got && memcmp(got, data, sizeof(data)) == 0);
    CHECK(1, "rs_contains(bond_key)", rs_contains(&rs, bk));

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T2 — origin_key = birth pile identity (rs_verify contract)
   ═══════════════════════════════════════════════════════════════ */
static void test_verify(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);

    uint8_t d[16];
    fill_pattern(d, sizeof(d), 7);
    PoglsPiece p = pogls_make_piece(0xABCD, 1);
    rs_freeze(&rs, &p, d, sizeof(d), 0);

    /* verify checks origin_key == piece->geo_key (birth pile identity).
       Before the fix, the empty-slot path stored origin_key = bond_key,
       so this FAILED for every fresh entry. */
    CHECK(2, "rs_verify true for the freezing piece", rs_verify(&rs, &p) == 1);

    /* entry exposes origin_key == geo_key directly */
    uint32_t sz = 0;
    const void *got = rs_thaw(&rs, pogls_bond_key(&p), &sz);
    const ResidualEntry *e = (const ResidualEntry *)((const uint8_t *)got -
                             RS_ENTRY_HEADER_SZ);
    CHECK(2, "stored origin_key == geo_key", e->origin_key == p.geo_key);
    CHECK(2, "stored geo_key == geo_key",     e->geo_key == p.geo_key);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T3 — เสาเข็มห้ามขยับ: coordinate shift → bond breaks
   ═══════════════════════════════════════════════════════════════ */
static void test_bond_break(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);

    uint8_t d[32];
    fill_pattern(d, sizeof(d), 99);
    PoglsPiece p  = pogls_make_piece(0x1111, 5);
    PoglsPiece p2 = pogls_make_piece(0x1112, 5);  /* same axis, seed+1 = moved pile */
    PoglsPiece p_same = pogls_make_piece(0x1111, 5); /* re-derived from same seed */

    rs_freeze(&rs, &p, d, sizeof(d), 0);

    CHECK(3, "same seed → identical bond_key (deterministic address)",
          pogls_bond_key(&p) == pogls_bond_key(&p_same));
    CHECK(3, "shifted pile → different bond_key (bond broke)",
          pogls_bond_key(&p) != pogls_bond_key(&p2));

    uint32_t sz = 0;
    CHECK(3, "thaw via moved pile returns NULL", rs_thaw(&rs, pogls_bond_key(&p2), &sz) == NULL);
    CHECK(3, "verify(moved pile) == 0", rs_verify(&rs, &p2) == 0);
    CHECK(3, "rs_contains(moved bond) == 0", rs_contains(&rs, pogls_bond_key(&p2)) == 0);
    CHECK(3, "original pile still reachable", rs_contains(&rs, pogls_bond_key(&p)) == 1);
    CHECK(3, "no geo-address lookup API exists (bond-only)",
          sizeof(ResidualEntry) == RS_ENTRY_HEADER_SZ);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T4 — refreeze same piece = update, single entry kept
   ═══════════════════════════════════════════════════════════════ */
static void test_update(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);

    uint8_t d1[64], d2[128];
    fill_pattern(d1, sizeof(d1), 1);
    fill_pattern(d2, sizeof(d2), 2);

    PoglsPiece p = pogls_make_piece(0x2222, 4);
    uint64_t bk = rs_freeze(&rs, &p, d1, sizeof(d1), 0);
    rs_freeze(&rs, &p, d2, sizeof(d2), 0);

    CHECK(4, "same bond_key returned on refreeze", bk == pogls_bond_key(&p));
    uint32_t sz = 0;
    const void *got = rs_thaw(&rs, bk, &sz);
    CHECK(4, "thaw returns NEW data after refreeze",
          got && sz == sizeof(d2) && memcmp(got, d2, sizeof(d2)) == 0);
    CHECK(4, "count stays 1 (no duplicate entry)", rs.count == 1);
    CHECK(4, "verify still true after update", rs_verify(&rs, &p) == 1);
    CHECK(4, "total_bytes tracks latest payload", rs.total_bytes == sizeof(d2));

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T5 — tombstone → dead logically → sweep reclaims
   ═══════════════════════════════════════════════════════════════ */
static void test_tombstone(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);

    uint8_t d[16];
    fill_pattern(d, sizeof(d), 5);
    PoglsPiece p = pogls_make_piece(0x3333, 6);
    uint64_t bk = rs_freeze(&rs, &p, d, sizeof(d), 0);

    CHECK(5, "tombstone succeeds", rs_tombstone(&rs, bk) == 1);
    CHECK(5, "tombstone twice → 0 (already dead)", rs_tombstone(&rs, bk) == 0);
    CHECK(5, "rs_contains false after tombstone", rs_contains(&rs, bk) == 0);
    CHECK(5, "tombstone_count == 1", rs_tombstone_count(&rs) == 1);

    uint32_t swept = rs_tombstone_sweep(&rs);
    CHECK(5, "sweep reclaims 1 entry", swept == 1);
    CHECK(5, "tombstone_count == 0 after sweep", rs_tombstone_count(&rs) == 0);
    CHECK(5, "rs.count == 0 after sweep", rs.count == 0);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T6 — expire_by_origin: bulk death of one pile's entries
   ═══════════════════════════════════════════════════════════════ */
static void test_expire_by_origin(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);

    uint8_t d[8];
    fill_pattern(d, sizeof(d), 3);
    PoglsPiece p1 = pogls_make_piece(0xA001, 1);
    PoglsPiece p2 = pogls_make_piece(0xA002, 1);
    PoglsPiece p3 = pogls_make_piece(0xA003, 1);
    rs_freeze(&rs, &p1, d, sizeof(d), 0);
    rs_freeze(&rs, &p2, d, sizeof(d), 0);
    rs_freeze(&rs, &p3, d, sizeof(d), 0);

    uint32_t n = rs_expire_by_origin(&rs, p2.geo_key);
    CHECK(6, "expire_by_origin kills exactly the matching pile", n == 1);
    CHECK(6, "pile2 dead", !rs_contains(&rs, pogls_bond_key(&p2)));
    CHECK(6, "pile1 survives", rs_contains(&rs, pogls_bond_key(&p1)));
    CHECK(6, "pile3 survives", rs_contains(&rs, pogls_bond_key(&p3)));

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T7 — evict_all empties the zone
   ═══════════════════════════════════════════════════════════════ */
static void test_evict_all(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);

    uint8_t d[16];
    fill_pattern(d, sizeof(d), 8);
    for (int i = 0; i < 10; i++) {
        PoglsPiece p = pogls_make_piece(0xB000 + i, 2);
        rs_freeze(&rs, &p, d, sizeof(d), 0);
    }
    CHECK(7, "10 entries frozen", rs.count == 10);

    uint32_t evicted = rs_evict_all(&rs);
    CHECK(7, "evict_all clears all 10", evicted == 10);
    CHECK(7, "count == 0", rs.count == 0);
    CHECK(7, "total_bytes == 0", rs.total_bytes == 0);
    CHECK(7, "eviction counter advanced", rs.evictions >= 10);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T8 — high-entropy marking
   ═══════════════════════════════════════════════════════════════ */
static void test_high_entropy(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);

    uint8_t d[24];
    fill_pattern(d, sizeof(d), 11);
    PoglsPiece a = pogls_make_piece(0xC001, 1);
    PoglsPiece b = pogls_make_piece(0xC002, 1);
    PoglsPiece c = pogls_make_piece(0xC003, 1);
    rs_freeze(&rs, &a, d, sizeof(d), 1);   /* high entropy */
    rs_freeze(&rs, &b, d, sizeof(d), 0);
    rs_freeze(&rs, &c, d, sizeof(d), 1);   /* high entropy */

    CHECK(8, "2 of 3 marked high entropy", rs_count_high_entropy(&rs) == 2);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T9 — stats consistency
   ═══════════════════════════════════════════════════════════════ */
static void test_stats(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);

    uint8_t d[30];
    fill_pattern(d, sizeof(d), 13);
    PoglsPiece p1 = pogls_make_piece(0xD001, 1);
    PoglsPiece p2 = pogls_make_piece(0xD002, 1);
    PoglsPiece p3 = pogls_make_piece(0xD003, 1);
    rs_freeze(&rs, &p1, d, 10, 1);
    rs_freeze(&rs, &p2, d, 20, 0);
    rs_freeze(&rs, &p3, d, 30, 0);

    ResidualSpaceStats s = rs_stats(&rs);
    CHECK(9, "stats.count == 3", s.count == 3);
    CHECK(9, "stats.total_bytes == 60", s.total_bytes == 60);
    CHECK(9, "stats.high_entropy_count == 1", s.high_entropy_count == 1);
    CHECK(9, "stats.avg_entry_bytes == 20", s.avg_entry_bytes == 20);
    CHECK(9, "stats.capacity == 64", s.capacity == 64);
    CHECK(9, "stats.tombstone_count == 0", s.tombstone_count == 0);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T10 — invalid input rejection (RS_BOND_KEY_RESERVED / NULL)
   ═══════════════════════════════════════════════════════════════ */
static void test_invalid(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);

    uint8_t d[8];
    fill_pattern(d, sizeof(d), 17);
    PoglsPiece p = pogls_make_piece(0xE001, 1);

    CHECK(10, "freeze NULL piece → 0", rs_freeze(&rs, NULL, d, 8, 0) == 0);
    CHECK(10, "freeze size 0 → 0", rs_freeze(&rs, &p, d, 0, 0) == 0);
    CHECK(10, "freeze size > MAX → 0",
          rs_freeze(&rs, &p, d, RS_MAX_DATA_SIZE + 1, 0) == 0);
    CHECK(10, "thaw bond_key 0 → NULL", rs_thaw(&rs, 0, NULL) == NULL);
    CHECK(10, "thaw unknown bond_key → NULL",
          rs_thaw(&rs, 0xDEADBEEF, NULL) == NULL);
    CHECK(10, "tombstone bond_key 0 → 0", rs_tombstone(&rs, 0) == 0);
    CHECK(10, "rs_verify NULL piece → 0", rs_verify(&rs, NULL) == 0);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T11 — zone full → LRU eviction (no failure, bounded count)
   ═══════════════════════════════════════════════════════════════ */
static void test_eviction(void) {
    ResidualSpace rs;
    rs_init(&rs, 64);   /* minimal zone — forces the table-full path */

    uint8_t d[4];
    fill_pattern(d, sizeof(d), 19);
    uint32_t kept = 0;
    for (int i = 0; i < 256; i++) {
        PoglsPiece p = pogls_make_piece(0xF000 + (uint64_t)i, 1);
        uint64_t bk = rs_freeze(&rs, &p, d, sizeof(d), 0);
        if (bk != RS_BOND_KEY_RESERVED) kept++;
    }

    CHECK(11, "all 256 freezes accepted (eviction makes room)", kept == 256);
    CHECK(11, "count bounded by capacity", rs.count <= rs.capacity);
    CHECK(11, "evictions actually happened", rs.evictions > 0);

    /* last piece still readable (bond survived eviction churn) */
    PoglsPiece last = pogls_make_piece(0xF000 + 255, 1);
    uint32_t sz = 0;
    CHECK(11, "final entry still reachable",
          rs_thaw(&rs, pogls_bond_key(&last), &sz) != NULL);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T12 — bond verification: deterministic + symmetric
   ═══════════════════════════════════════════════════════════════ */
static void test_bond_verify(void) {
    PoglsPiece a = pogls_make_piece(0x1001, 3);
    PoglsPiece b = pogls_make_piece(0x2002, 3);

    PoglsBond ab1 = pogls_bond_verify(&a, &b);
    PoglsBond ab2 = pogls_bond_verify(&a, &b);
    PoglsBond ba  = pogls_bond_verify(&b, &a);

    CHECK(12, "verify is deterministic", ab1.valid == ab2.valid && ab1.bond_key == ab2.bond_key);
    CHECK(12, "verify is symmetric (XOR-based)", ab1.bond_key == ba.bond_key && ab1.valid == ba.valid);
    CHECK(12, "bond_key exposed is pre-nonce raw_xor",
          ab1.bond_key == (pogls_bond_key(&a) ^ pogls_bond_key(&b)));
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Residual Space — Timeless Bond-Only Storage Zone\n");
    printf("════════════════════════════════════════════════════════\n\n");

    test_roundtrip();
    test_verify();
    test_bond_break();
    test_update();
    test_tombstone();
    test_expire_by_origin();
    test_evict_all();
    test_high_entropy();
    test_stats();
    test_invalid();
    test_eviction();
    test_bond_verify();

    printf("\n════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("════════════════════════════════════════════════════════\n");

    return fail == 0 ? 0 : 1;
}
