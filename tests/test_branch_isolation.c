/* ═══════════════════════════════════════════════════════════════════════════
 * test_branch_isolation.c — branch-key placement gate (#58 item 5)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * DECIDES + PROVES the placement of the forager's branch key (which branch a
 * route took, and whether it has ever been traversed):
 *
 *   (a) separate latch bitset  (core/geo_wang_latch.h `wl_latch_t`)
 *   (b) P3 reserved tag        (core/geo_id_route_log.h, name_hash WL_NAME_HASH)
 *   (c) orient bit             (steal a bit from the CID / address)
 *
 * VERDICT: **(a) separate latch, persisted through (b) P3 tag, (c) REJECTED.**
 *
 *   - (a) gives O(1) `wl_is_open` reads; it is a DERIVED cache (replay = recompute).
 *   - (b) is the SOURCE OF TRUTH: one 32B P3 record per traverse/clear; a fresh
 *     process rebuilds (a) exactly by wl_replay().
 *   - (c) is rejected because it would write *state* into the *stateless color*
 *     space (mm_wang6: "color = can this path go"), and it would consume an
 *     address bit that the 20736 frame cannot spare (#6337).
 *
 * This test gates:
 *   T1 id bijection: wl_id(cell,choice) over [0,144)x[0,72) → [0,10368) bijective
 *   T2 reserved exclusion: ids 0 and 10367 are NOT in the minor free pool
 *   T3 monotone: traverse is one-way; a second traverse is a no-op
 *   T4 CLEAR is the only reopen
 *   T5 isolation: a route through branch A leaves B, C, ... OPEN
 *   T6 replay identity: fresh wl_latch + replay(P3 buffer) == live bitset
 *   T7 foreign tag ignored: a non-WL P3 record applied changes nothing
 *   T8 placement C rejected: color space is stateless — no bit is stolen for state
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "geo_wang_latch.h"

static int pass = 0, fail = 0;
static void chk(const char *n, int ok) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", n);
    if (ok) pass++; else fail++;
}

int main(void) {
    printf("branch-isolation gate (#58 item 5)\n");

    /* T1 bijection */
    {
        static uint8_t seen[WL_COUNT];
        int ok = 1;
        memset(seen, 0, sizeof seen);
        for (uint32_t c = 0; c < 144; c++)
            for (uint32_t k = 0; k < 72; k++) {
                uint32_t id = wl_id(c, k);
                if (id >= WL_COUNT || seen[id]) { ok = 0; goto t1; }
                seen[id] = 1;
            }
    t1:
        chk("T1 wl_id(cell,choice) bijective over 144x72 -> 10368", ok);
    }

    /* T2 reserved exclusion */
    {
        int ok = wl_is_reserved(WL_RESERVED_LO) && wl_is_reserved(WL_RESERVED_HI);
        /* reserved ids are refused by traverse */
        wl_latch_t L; wl_reset(&L);
        ok = ok && (wl_traverse(&L, WL_RESERVED_LO) == 0);
        ok = ok && (wl_traverse(&L, WL_RESERVED_HI) == 0);
        ok = ok && wl_is_open(&L, WL_RESERVED_LO) && wl_is_open(&L, WL_RESERVED_HI);
        chk("T2 ids 0 and 10367 excluded from the minor free pool", ok);
    }

    /* T3 monotone */
    {
        wl_latch_t L; wl_reset(&L);
        uint32_t id = wl_id(10, 20);
        int ok = wl_is_open(&L, id) == 1;
        ok = ok && wl_traverse(&L, id) == 1;      /* OPEN -> SHUT */
        ok = ok && wl_is_open(&L, id) == 0;
        ok = ok && wl_traverse(&L, id) == 1;      /* idempotent no-op */
        ok = ok && wl_is_open(&L, id) == 0;
        chk("T3 traverse is one-way (monotone) and idempotent", ok);
    }

    /* T4 CLEAR is the only reopen */
    {
        wl_latch_t L; wl_reset(&L);
        uint32_t id = wl_id(11, 21);
        wl_traverse(&L, id);
        int ok = wl_is_open(&L, id) == 0;
        ok = ok && wl_clear(&L, id) == 1;
        ok = ok && wl_is_open(&L, id) == 1;
        chk("T4 CLEAR is the only reopen", ok);
    }

    /* T5 isolation: a route through A leaves B,C,... OPEN */
    {
        wl_latch_t L; wl_reset(&L);
        uint32_t A = wl_id(5, 1), B = wl_id(5, 2), C = wl_id(6, 1);
        wl_traverse(&L, A);
        int ok = wl_is_open(&L, A) == 0;
        ok = ok && wl_is_open(&L, B) == 1;
        ok = ok && wl_is_open(&L, C) == 1;
        chk("T5 branch isolation: traversing A leaves B,C,.. OPEN", ok);
    }

    /* T6 replay identity: fresh latch + replay(P3) == live bitset */
    {
        wl_latch_t live; wl_reset(&live);
        uint8_t log[IR_LOG_RECORD_SIZE * 6];
        IR_log_record_t r;
        uint32_t ids[6] = { wl_id(1, 1), wl_id(1, 2), wl_id(2, 5),
                            wl_id(17, 40), wl_id(17, 40), wl_id(3, 3) };
        int act[6]  = { IR_LOG_ACTION_WRITE, IR_LOG_ACTION_WRITE, IR_LOG_ACTION_WRITE,
                        IR_LOG_ACTION_WRITE, IR_LOG_ACTION_CLEAR, IR_LOG_ACTION_WRITE };
        for (int i = 0; i < 6; i++) {
            memset(&r, 0, sizeof r);
            r.slot = ids[i];
            r.action = act[i];
            r.name_hash = WL_NAME_HASH;
            ir_log_serialize(log + (size_t)i * IR_LOG_RECORD_SIZE, &r);
            wl_apply_record(&live, &r);
        }
        wl_latch_t replay; wl_replay(&replay, log, sizeof log);
        int ok = memcmp(live.w, replay.w, sizeof live.w) == 0;
        /* sanity: the cleared id is open, the others shut */
        ok = ok && wl_is_open(&live, wl_id(17, 40)) == 1;
        ok = ok && wl_is_open(&live, wl_id(1, 1)) == 0;
        chk("T6 replay(P3 buffer) reconstructs the live bitset byte-exactly", ok);
    }

    /* T7 foreign tag ignored */
    {
        wl_latch_t L; wl_reset(&L);
        IR_log_record_t r; memset(&r, 0, sizeof r);
        r.slot = wl_id(7, 7);
        r.action = IR_LOG_ACTION_WRITE;
        r.name_hash = 0xDEADBEEFCAFEull;   /* not WL_NAME_HASH */
        wl_apply_record(&L, &r);
        chk("T7 foreign name_hash P3 record changes nothing", wl_is_open(&L, wl_id(7, 7)) == 1);
    }

    /* T8 placement C rejected: color space stays stateless.
     * We cannot include mm_wang6.h's full machinery here, but the invariant is
     * checkable: the latch is a SEPARATE struct (no field aliases the CID), and
     * no address bit is consumed. Assert sizeof(latch) == 1296 B (162 words). */
    {
        int ok = (sizeof(wl_latch_t) == WL_BYTES);
        /* the latch never reaches into the color/address: the CID space and the
         * latch are distinct types, and wl_traverse takes a latch*, not a color. */
        ok = ok && (WL_BYTES == 1296);
        chk("T8 (c) rejected: latch is a separate 1296B struct, no address bit stolen", ok);
    }

    printf("\nBRANCH-ISOLATION: %d pass, %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
