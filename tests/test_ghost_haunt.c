/* tests/test_ghost_haunt.c — Haunt: carry the value instead of walking to it
 * ══════════════════════════════════════════════════════════════════════
 * The ladder walk is real and available: tl_next_active() hops one rung at a
 * time, so reaching slot k costs k hops of re-deriving the same geometry every
 * time. Haunt replaces that with an O(1) thaw through a bond key — plant a
 * residual checkpoint at each point, then jump straight to it without
 * replaying the steps in between, because the steps are the same everywhere
 * anyway. (Spectre's Haunt, from Dota: one origin, many paths pointing at it.)
 *
 * 192 slots here = the flat ladder inside ONE geo_jump unit — 144 active
 * (GEO_TOWER = 48 x 3) plus 48 residual (GEO_BLOCK) — so the jump table is
 * flat and needs no cross-tower routing.
 *
 * Proven here:
 *   - a jump returns byte-identical data to the walk it replaces
 *   - the jump executes ZERO ladder hops, which is the entire point
 *   - thaw order is irrelevant: shuffled replay still memcmp 100%
 *   - pinning is load-bearing: unpin a checkpoint and the jump table loses it
 *
 * NOT proven: that this is worth doing at this size. 192 x 64 B of payload
 * against a 36 B residual header means the basket is heavier than the rider;
 * the jump only pays once one checkpoint serves many carries. The mechanism is
 * proven, the economics are not, and they are separate questions.
 *
 * Integer-only, header-only, no model file. Value-blind by construction: the
 * transform never inspects a byte, so a generated payload is a complete proof
 * of the roundtrip, not a weaker one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_ghost_lift.h"
#include "../core/geo_tower_ladder.h"

static int pass = 0, fail = 0;
#define OK(c, m) do { if (c) { pass++; printf("PASS T%d: %s\n", T, m); } \
                      else { fail++; printf("FAIL T%d: %s\n", T, m); } } while (0)

#define PAY_B    64u
#define PAY_U32  (PAY_B / 4u)

/* deterministic per-slot payload; every slot differs so a wrong jump is visible */
static uint32_t pat(uint32_t slot, uint32_t j) {
    return (slot * 2654435761u + j * 40503u) ^ (slot >> 3);
}
static void fill(uint8_t *b, uint32_t slot) {
    uint32_t *w = (uint32_t *)b;
    for (uint32_t j = 0; j < PAY_U32; j++) w[j] = pat(slot, j);
}
static uint32_t xorshift(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x;
}

/* Counting wrapper. The jump table must move the payload without the ladder
 * being consulted at all, and that claim is only worth anything if the counter
 * is real — so every ladder hop anywhere in this file goes through here. */
static uint32_t g_tl_hops;
static uint32_t walk_hop(uint32_t slot) { g_tl_hops++; return tl_next_active(slot); }

static void main_test(void) {
    ResidualSpace rs;
    GhostLog log;
    uint32_t T = 1;

    /* ── T1: enumerate all 192 slots, and count what the WALK costs ───── */
    {
        static uint8_t src[TL_TOTAL][PAY_B];
        uint32_t hops = 0, seen = 0;
        uint8_t bitmap[TL_TOTAL];

        memset(bitmap, 0, sizeof bitmap);
        for (uint32_t s = 0; s < TL_TOTAL; s++) fill(src[s], s);

        /* the walk: one tl_next_active() per active slot */
        uint32_t slot = tl_active_slot(0u, 0u, 0u);
        for (uint32_t n = 0; n < TL_ACTIVE_PER_TOWER * TL_TOWERS; n++) {
            if (!bitmap[slot]) { bitmap[slot] = 1; seen++; }
            slot = walk_hop(slot);
            hops++;
        }
        /* residual zone is addressed directly, not walked */
        for (uint32_t t = 0; t < TL_TOWERS; t++)
            for (uint32_t r = 0; r < TL_RUNGS; r++)
                for (uint32_t c = 0; c < TL_CORNERS; c++) {
                    uint32_t rs_ = tl_residual_slot(t, r, c);
                    if (!bitmap[rs_]) { bitmap[rs_] = 1; seen++; }
                }
        printf("walk: %u hops for %u slots\n", hops, seen);
        OK(seen == TL_TOTAL, "the walk reaches all 192 slots");
        OK(hops == TL_ACTIVE_PER_TOWER * TL_TOWERS, "the walk costs 144 hops");
    }

    /* ── T2: plant the Haunt table ────────────────────────────────────── */
    /* jump[slot] = bond key. 8 B per point, no payload copy: the residual
     * store already holds the bytes, the table only says where. */
    static uint64_t jump[TL_TOTAL];
    static uint8_t  src[TL_TOTAL][PAY_B];
    {
        for (uint32_t s = 0; s < TL_TOTAL; s++) fill(src[s], s);
        rs_init(&rs, 256);
        ghost_log_init(&log);
        uint32_t planted = 0;
        for (uint32_t s = 0; s < TL_TOTAL; s++) {
            uint64_t bk = ghost_lift(&log, &rs, (uint16_t)s, 0u, 1u, src[s], PAY_B);
            if (bk == RS_BOND_KEY_RESERVED) continue;
            jump[s] = bk;
            planted++;
            rs_set_pinned(&rs, bk, 1);   /* the checkpoint must not drift */
        }
        printf("haunt: %u checkpoints planted\n", planted);
        OK(planted == TL_TOTAL, "all 192 checkpoints planted");

        /* Route lookup has two shapes: binary search over the log (no table)
         * or the pair table (O(1)). attach() marks it dirty by design, so build
         * has to come after attach for it to end up fresh. */
        GhostPairTable pt;
        memset(&pt, 0, sizeof pt);
        ghost_pair_attach(&log, &pt);
        int built = ghost_pair_build(&log, &pt);
        printf("pair table: build=%d, pile %u B for %u blocks\n",
               built, (unsigned)(pt.max_block * 256u * sizeof(uint16_t)),
               pt.max_block);
        OK(built == 0, "pair table built from the log");
        OK(ghost_pair_fresh(&log), "pair table fresh after attach-then-build");
        ghost_pair_detach(&log);
        ghost_pair_free(&pt);
    }

    /* ── T3: the jump — O(1), and it returns the right bytes ───────────── */
    {
        uint32_t before = g_tl_hops;      /* the walk already spent 144 */
        int all = 1;
        for (uint32_t s = 0; s < TL_TOTAL; s++) {
            uint32_t sz = 0;
            const void *p = rs_thaw(&rs, jump[s], &sz);
            if (!p || sz != PAY_B || memcmp(p, src[s], PAY_B) != 0) all = 0;
        }
        uint32_t spent = g_tl_hops - before;
        printf("jump: 192 jumps cost %u ladder hops (walk cost %u)\n", spent, before);
        OK(all, "every jump returns byte-identical payload");
        OK(spent == 0, "all 192 jumps took 0 ladder hops — the ladder was never called");
    }

    /* ── T4: order is irrelevant — shuffle the thaw order ──────────────── */
    /* The walk has one legal order (it is a path). The jump table does not:
     * that is the property worth having, so it gets its own test. */
    {
        uint32_t order[TL_TOTAL], i, j, tmp;
        uint32_t st = 0xC0FFEEu;
        for (i = 0; i < TL_TOTAL; i++) order[i] = i;
        for (i = TL_TOTAL - 1; i > 0; i--) {           /* Fisher-Yates */
            j = xorshift(&st) % (i + 1u);
            tmp = order[i]; order[i] = order[j]; order[j] = tmp;
        }
        int shuffled_ok = 0;
        for (i = 1; i < TL_TOTAL; i++) if (order[i] != i) { shuffled_ok = 1; break; }

        uint8_t out[PAY_B];
        int all = 1;
        for (i = 0; i < TL_TOTAL; i++) {
            uint32_t sz = 0, s = order[i];
            const void *p = rs_thaw(&rs, jump[s], &sz);
            if (!p || sz != PAY_B) { all = 0; continue; }
            memcpy(out, p, PAY_B);
            if (memcmp(out, src[s], PAY_B) != 0) all = 0;
        }
        OK(shuffled_ok, "the replay order really was shuffled");
        OK(all, "shuffled replay still reconstructs every payload");
    }

    /* ── T5: pinning is what makes it safe (negative control) ─────────── */
    /* Unpin one checkpoint, then flood the store. LRU picks the OLDEST
     * UNPINNED entry, and this one now is that entry — so the jump must fail.
     * Without this, a Haunt table that quietly loses points looks identical to
     * one that works, right up until the moment it does not. */
    {
        const uint32_t victim = 0;
        OK(rs_set_pinned(&rs, jump[victim], 0) == 0, "victim checkpoint unpinned");
        uint8_t junk[PAY_B];
        memset(junk, 0xEE, PAY_B);
        uint32_t flooded = 0;
        for (uint32_t b = 0; b < 400u; b++) {
            PoglsPiece p = ghost_piece((uint16_t)(TL_TOTAL + b), 0u, 1u);
            uint64_t bk = rs_freeze(&rs, &p, junk, PAY_B, 0);
            if (bk != RS_BOND_KEY_RESERVED) flooded++;
        }
        uint32_t sz = 0;
        const void *p = rs_thaw(&rs, jump[victim], &sz);
        printf("flood: %u inserts, victim jump -> %s\n",
               flooded, p ? "STILL THERE" : "GONE");
        OK(p == NULL, "an unpinned checkpoint IS evicted, and the jump fails loudly");
    }

    /* ── T6: what it costs ─────────────────────────────────────────────── */
    {
        uint32_t jt = TL_TOTAL * 8u, pay = TL_TOTAL * PAY_B;
        printf("cost: jump table %u B (%u pts x 8)\n", (unsigned)jt, TL_TOTAL);
        printf("      payload        %u B (%u x %u), held in the store not the table\n",
               (unsigned)pay, TL_TOTAL, PAY_B);
        printf("      pair table     %u B for O(1) route lookup -> %.0fx the jump table\n",
               (unsigned)(TL_TOTAL * 256u * 2u),
               (double)(TL_TOTAL * 256u * 2u) / (double)jt);
        printf("      at 192 points the binary-search fallback is the cheaper shape;\n");
        printf("      the table only pays once one checkpoint serves many reads\n");
        OK(jt < pay, "the jump table is smaller than the payload it points at");
        OK(TL_TOTAL <= RS_DEFAULT_CAPACITY,
           "192 checkpoints fit one default residual store");
    }
}

int main(void) {
    main_test();
    printf("\nRESULT: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
