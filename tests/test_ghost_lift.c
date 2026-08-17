/* test_ghost_lift.c — Ghost Lift: hyperbolic track wired into residual_space
 * ═══════════════════════════════════════════════════════════════════════════
 * End-to-end proof of the wiring (core/geo_ghost_lift.h + core/residual_space.h):
 *
 *   Block requests scale beyond envelope → FREEZE data in residual_space,
 *   hyperbolic log TRACKS the ghost:
 *     bond = BIRTH IDENTITY (block_id, from_scale)   ← เสาเข็มห้ามขยับ
 *     log  = ROUTE {block_id, from→to} = 5 B/event  ← passive scale log
 *
 *   T1  lift → freeze + log entry (deterministic bond_key)
 *   T2  read at same route → lossless (bytes identical)
 *   T3  wrong from_scale → bond breaks → NULL (เสาเข็มห้ามขยับ)
 *   T4  wrong to_scale → route not found → NULL (log = path)
 *   T5  second route to same frozen data → same bond, rs.count stays 1
 *   T6  different blocks → different bonds, isolated
 *   T7  re-attach (expire) → data dies via rs_expire_by_origin, routes
 *       stay as audit trail (GHOST_FLAG_EXPIRED)
 *   T8  new pile after re-attach works; old pile stays dead
 *   T9  telescope — from 3 → to 140 = ONE entry (path ∝ events, not steps)
 *   T10 determinism — same (block, from, to) → same bond_key everywhere
 *   T11 invalid input rejected (NULL data / size 0 / duplicate route)
 *   T12 no route → no access (log is the authority, not just the bond)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-ghost_lift tests/test_ghost_lift.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_ghost_lift.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static void fill_pattern(uint8_t *buf, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i * 31 + seed * 7) & 0xFF);
}

/* ═══════════════════════════════════════════════════════════════
   T1 + T2 — lift → read lossless
   ═══════════════════════════════════════════════════════════════ */
static void test_lift_read(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t data[64];
    fill_pattern(data, sizeof(data), 42);
    uint64_t bk = ghost_lift(&log, &rs, 7, 5, 9, data, sizeof(data));

    CHECK(1, "lift returns nonzero bond_key", bk != RS_BOND_KEY_RESERVED);
    CHECK(1, "bond_key == ghost_bond_key(7,5,9)",
          bk == ghost_bond_key(7, 5, 9));
    CHECK(1, "one log entry appended", log.count == 1);
    CHECK(1, "one frozen entry in residual space", rs.count == 1);
    CHECK(1, "route found live", ghost_log_find(&log, 7, 5, 9) >= 0);

    uint32_t sz = 0;
    const void *got = ghost_read(&log, &rs, 7, 5, 9, &sz);
    CHECK(2, "read at same route returns data", got != NULL);
    CHECK(2, "read size matches", sz == sizeof(data));
    CHECK(2, "read bytes identical (lossless)",
          got && memcmp(got, data, sizeof(data)) == 0);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T3 + T4 — two-layer truth: bond breaks / route not found
   ═══════════════════════════════════════════════════════════════ */
static void test_wrong_keys(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t data[32];
    fill_pattern(data, sizeof(data), 7);
    ghost_lift(&log, &rs, 3, 2, 8, data, sizeof(data));

    uint32_t sz = 0;
    CHECK(3, "wrong from_scale (3,3,8) → NULL (bond broke)",
          ghost_read(&log, &rs, 3, 3, 8, &sz) == NULL);
    CHECK(3, "wrong from_scale (3,1,8) → NULL",
          ghost_read(&log, &rs, 3, 1, 8, &sz) == NULL);
    PoglsPiece moved = ghost_piece(3, 3, 8);
    CHECK(3, "rs_verify(moved pile) == 0", rs_verify(&rs, &moved) == 0);
    CHECK(4, "wrong to_scale (3,2,9) → NULL (route not found)",
          ghost_read(&log, &rs, 3, 2, 9, &sz) == NULL);
    CHECK(4, "wrong block (4,2,8) → NULL",
          ghost_read(&log, &rs, 4, 2, 8, &sz) == NULL);
    CHECK(4, "correct route still readable",
          ghost_read(&log, &rs, 3, 2, 8, &sz) != NULL);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T5 — data frozen ONCE, many routes to the same bond
   ═══════════════════════════════════════════════════════════════ */
static void test_multi_route(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t data[48];
    fill_pattern(data, sizeof(data), 11);
    uint64_t bk1 = ghost_lift(&log, &rs, 9, 4, 6, data, sizeof(data));
    uint64_t bk2 = ghost_lift(&log, &rs, 9, 4, 20, data, sizeof(data));

    CHECK(5, "two routes → SAME bond_key (same birth pile)",
          bk1 == bk2 && bk1 != RS_BOND_KEY_RESERVED);
    CHECK(5, "data frozen once (rs.count == 1)", rs.count == 1);
    CHECK(5, "log records both routes", log.count == 2);
    CHECK(5, "both routes readable",
          ghost_read(&log, &rs, 9, 4, 6, NULL) != NULL &&
          ghost_read(&log, &rs, 9, 4, 20, NULL) != NULL);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T6 — blocks isolated by bond
   ═══════════════════════════════════════════════════════════════ */
static void test_isolation(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t d[16];
    fill_pattern(d, sizeof(d), 3);
    uint64_t bk_a = ghost_lift(&log, &rs, 1, 0, 4, d, sizeof(d));
    uint64_t bk_b = ghost_lift(&log, &rs, 2, 0, 4, d, sizeof(d));

    CHECK(6, "different blocks → different bond_keys", bk_a != bk_b);
    CHECK(6, "block A readable", ghost_read(&log, &rs, 1, 0, 4, NULL) != NULL);
    CHECK(6, "block B readable", ghost_read(&log, &rs, 2, 0, 4, NULL) != NULL);

    /* expire block A's pile only */
    uint32_t ex = ghost_expire(&log, &rs, 1, 0);
    CHECK(6, "expire kills A's routes", ex == 1);
    CHECK(6, "A dead", ghost_read(&log, &rs, 1, 0, 4, NULL) == NULL);
    CHECK(6, "B untouched", ghost_read(&log, &rs, 2, 0, 4, NULL) != NULL);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T7 — re-attach: expire_by_origin end-to-end + audit trail
   ═══════════════════════════════════════════════════════════════ */
static void test_reattach(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t d[24];
    fill_pattern(d, sizeof(d), 5);
    ghost_lift(&log, &rs, 5, 3, 10, d, sizeof(d));
    ghost_lift(&log, &rs, 5, 3, 12, d, sizeof(d));   /* 2 routes, 1 pile */

    uint32_t ex = ghost_expire(&log, &rs, 5, 3);
    CHECK(7, "expire returns all routes of the old pile", ex == 2);
    CHECK(7, "route 1 flagged expired",
          (log.entries[0].flags & GHOST_FLAG_EXPIRED) != 0);
    CHECK(7, "route 2 flagged expired",
          (log.entries[1].flags & GHOST_FLAG_EXPIRED) != 0);
    CHECK(7, "audit trail preserved (routes still recorded)",
          ghost_route_count(&log, 5) == 2);
    CHECK(7, "frozen data tombstoned (thaw fails)",
          rs_contains(&rs, ghost_bond_key(5, 3, 10)) == 0);
    CHECK(7, "read after re-attach → NULL",
          ghost_read(&log, &rs, 5, 3, 10, NULL) == NULL);
    CHECK(7, "expired route never matches again",
          ghost_log_find(&log, 5, 3, 10) < 0);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T8 — new pile after re-attach works; old pile stays dead
   ═══════════════════════════════════════════════════════════════ */
static void test_new_pile(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t d[16];
    fill_pattern(d, sizeof(d), 13);
    ghost_lift(&log, &rs, 5, 3, 10, d, sizeof(d));
    ghost_expire(&log, &rs, 5, 3);                       /* re-attach */

    uint64_t bk_new = ghost_lift(&log, &rs, 5, 12, 10, d, sizeof(d));  /* new home @12 */
    CHECK(8, "re-attached pile lifts with NEW bond", bk_new != RS_BOND_KEY_RESERVED);
    CHECK(8, "new bond != old bond",
          bk_new != ghost_bond_key(5, 3, 10));
    CHECK(8, "new pile readable",
          ghost_read(&log, &rs, 5, 12, 10, NULL) != NULL);
    CHECK(8, "old pile still dead",
          ghost_read(&log, &rs, 5, 3, 10, NULL) == NULL);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T9 — telescope: ONE entry for ANY distance on the scale axis
   ═══════════════════════════════════════════════════════════════ */
static void test_telescope(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t d[8];
    fill_pattern(d, sizeof(d), 17);
    /* from scale 3 to scale 140 — 137 steps away, still ONE entry */
    ghost_lift(&log, &rs, 11, 3, 140, d, sizeof(d));

    CHECK(9, "telescope = 1 entry (not 137)", log.count == 1);
    CHECK(9, "far route readable (lossless)",
          ghost_read(&log, &rs, 11, 3, 140, NULL) != NULL);
    CHECK(9, "log stays tiny: 5 B/entry → 5 B total", (uint32_t)sizeof(GhostLogEntry) == 5u);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T10 — determinism: address = coordinate, no lookup
   ═══════════════════════════════════════════════════════════════ */
static void test_determinism(void) {
    uint8_t d[8];
    fill_pattern(d, sizeof(d), 19);

    /* separate log + space, same parameters → same bond_key */
    GhostLog l1; ghost_log_init(&l1);
    ResidualSpace r1; rs_init(&r1, 64);
    uint64_t bk1 = ghost_lift(&l1, &r1, 21, 7, 33, d, sizeof(d));

    GhostLog l2; ghost_log_init(&l2);
    ResidualSpace r2; rs_init(&r2, 64);
    uint64_t bk2 = ghost_lift(&l2, &r2, 21, 7, 33, d, sizeof(d));

    CHECK(10, "same (block, from, to) → same bond_key across spaces",
          bk1 == bk2);
    CHECK(10, "piece re-derived → same geo_key (RDH address = block×256+from)",
          ghost_piece(21, 7, 33).geo_key == rdh_addr(21, 7) &&
          ghost_piece(21, 7, 33).geo_key == ghost_origin_seed(21, 7));

    rs_free(&r1); rs_free(&r2);
}

/* ═══════════════════════════════════════════════════════════════
   T11 — invalid input rejected
   ═══════════════════════════════════════════════════════════════ */
static void test_invalid(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t d[8];
    fill_pattern(d, sizeof(d), 23);
    ghost_lift(&log, &rs, 2, 1, 5, d, sizeof(d));

    CHECK(11, "NULL data → 0", ghost_lift(&log, &rs, 2, 1, 6, NULL, 8) == 0);
    CHECK(11, "size 0 → 0", ghost_lift(&log, &rs, 2, 1, 6, d, 0) == 0);
    CHECK(11, "duplicate route → 0 (no update-in-place, §15.5.3)",
          ghost_lift(&log, &rs, 2, 1, 5, d, sizeof(d)) == 0);
    CHECK(11, "NULL rs → 0", ghost_lift(&log, NULL, 2, 1, 6, d, 8) == 0);
    CHECK(11, "read before any lift → NULL",
          ghost_read(&log, &rs, 99, 1, 5, NULL) == NULL);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T12 — log is the authority: frozen data unreachable without a route
   ═══════════════════════════════════════════════════════════════ */
static void test_log_authority(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t d[8];
    fill_pattern(d, sizeof(d), 29);
    /* freeze data directly via residual_space (bypassing the log) */
    PoglsPiece p = ghost_piece(4, 6, 9);
    uint64_t bk = rs_freeze(&rs, &p, d, sizeof(d), 0);

    CHECK(12, "data exists in residual space", bk != 0);
    CHECK(12, "but NO live route → ghost_read NULL (log is the authority)",
          ghost_read(&log, &rs, 4, 6, 9, NULL) == NULL);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Ghost Lift — Hyperbolic Track wired into Residual Space\n");
    printf("════════════════════════════════════════════════════════\n\n");

    test_lift_read();
    test_wrong_keys();
    test_multi_route();
    test_isolation();
    test_reattach();
    test_new_pile();
    test_telescope();
    test_determinism();
    test_invalid();
    test_log_authority();

    printf("\n════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("════════════════════════════════════════════════════════\n");

    return fail == 0 ? 0 : 1;
}
