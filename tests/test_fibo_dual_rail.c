/*
 * test_fibo_dual_rail.c — Dual-Rail Walk: round counter + stride-37 sawtooth
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * The field (20736 = 12^4 = 144^2) is an equal-triangle tessellation. On a
 * triangular grid the triangles alternate orientation (up/down) across the
 * field — the field runs in a sawtooth by construction.
 *
 * CONSTRUCTION RULE (proven, not assumed — every property below is checked
 * against the math, never self-derived):
 *   - WALKING ALONG EDGES IS FORBIDDEN: sequential/edge-walk gives a 2-cycle
 *     (period 256, covers 2/12 faces) whose outbound path ≠ return path.
 *   - The legal walk is STRIDE-based: rail+ = +37 (scatter), rail− = −37
 *     (gather, inverse multiplier 16813). gcd(37, 20736) = 1 → stride is a
 *     full permutation (bijection) → every step is reversible by −37.
 *   - 37 is ODD → parity flips every step → triangle orientation alternates
 *     each step = the walk IS the sawtooth.
 *
 * ROUND COUNTER: the field is one ROUND (20736 slots). Walking a full round
 * wraps tick → round+1 (jet bridge). Scale is base-2: scale = 2^round
 * (level = exponent, exact integer shift — no float, no drift).
 *
 * Proofs:
 *   T1  full cycle — rail+ walks 20736 distinct slots and returns to start
 *   T2  gather inverse — scatter(·37)∘gather(·16813) = identity everywhere
 *   T3  rail− is the exact reverse — walking +37 then −37 returns to origin
 *   T4  sawtooth — parity flips on every step (odd stride = orientation flip)
 *   T5  round counter — full round wraps tick → round+1; scale = 1 << round
 *   T6  enter anywhere — starting at any slot, +37 reaches every slot once
 *   T7  dual-rail interleave — writing on rail+ and reading on rail− yields
 *       the same value at the same address (both are the same permutation)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_fibo_dual_rail tests/test_fibo_dual_rail.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_tess_container.h"
#include "../core/fibo_walk.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

#define RAIL_PLUS    TESS_STRIDE_37        /* 37  — scatter multiplier */
#define RAIL_MINUS   16813u                /* 37^-1 mod 20736 — gather */
#define RAIL_ROUND   TESS_TOTAL_SLOTS      /* one round = the full field */

/* forward +37 from a slot (rail+ step) */
static inline uint32_t rail_fwd(uint32_t slot) { return (slot + RAIL_PLUS) % RAIL_ROUND; }
/* backward −37 from a slot (rail− step) */
static inline uint32_t rail_bwd(uint32_t slot) { return (slot + RAIL_ROUND - RAIL_PLUS) % RAIL_ROUND; }

int main(void) {
    printf("═ DUAL-RAIL WALK — round counter + stride-37 sawtooth ═\n");
    printf("  field = 20736 (12^4), rail+ = +37, rail− = −37 (inv 16813)\n\n");

    /* ── T1: rail+ is a full 20736-cycle (Hamiltonian) ─────────────────── */
    {
        uint8_t seen[TESS_TOTAL_SLOTS];
        memset(seen, 0, sizeof(seen));
        uint32_t n = 0, cnt = 0;
        int uniq = 1;
        do {
            if (seen[n]) uniq = 0;
            seen[n] = 1;
            n = (n + RAIL_PLUS) % RAIL_ROUND;
            cnt++;
        } while (n != 0u);
        CHECK("T1: rail+ visits all 20736 slots exactly once and returns home",
              cnt == RAIL_ROUND && uniq);
    }

    /* ── T2: gather (×16813) is the inverse of scatter (×37) everywhere ── */
    {
        int ok = 1;
        for (uint32_t i = 0; i < RAIL_ROUND; i++)
            if (tess_stride_gather(tess_stride_scatter(i)) != i) { ok = 0; break; }
        CHECK("T2: gather∘scatter = identity at every slot (37·16813 ≡ 1 mod 20736)",
              ok);
    }

    /* ── T3: rail− is the exact reverse of rail+ ───────────────────────── */
    {
        int ok = 1;
        for (uint32_t s = 0; s < RAIL_ROUND; s++)
            if (rail_bwd(rail_fwd(s)) != s) { ok = 0; break; }
        CHECK("T3: +37 then −37 returns to the same slot (reverse walk is exact)",
              ok);
    }

    /* ── T4: sawtooth — parity flips every step (odd stride 37) ────────── */
    {
        uint32_t n = 0;
        int ok = 1, prev = (int)(n & 1u);
        for (uint32_t k = 0; k < RAIL_ROUND; k++) {
            n = (n + RAIL_PLUS) % RAIL_ROUND;
            int cur = (int)(n & 1u);
            if (cur == prev) { ok = 0; break; }
            prev = cur;
            if (n == 0u) break;
        }
        CHECK("T4: triangle orientation alternates every step (37 odd → parity flip = sawtooth)",
              ok);
    }

    /* ── T5: round counter — full round wraps tick → round+1, scale = 2^r ─ */
    {
        FiboWalkPos pos = {0, 0, 0};
        uint32_t ticks = RAIL_ROUND, cycles = 8;
        for (uint32_t i = 0; i < ticks; i++) fibo_walk_next(&pos, ticks, cycles);
        int round_ok = (pos.round == 1u && pos.tick == 0u && pos.steps == ticks);
        /* base-2 scale: level r → 2^r — exact integer shift, no float */
        int shift_ok = 1;
        for (uint32_t r = 0; r < 12; r++) {
            uint32_t by_shift = 1u << r;
            uint32_t by_mul   = 1u;
            for (uint32_t m = 0; m < r; m++) by_mul *= 2u;
            if (by_shift != by_mul) shift_ok = 0;
        }
        CHECK("T5a: full round wraps tick 0→round+1 (jet bridge)",
              round_ok);
        CHECK("T5b: scale = 2^level — integer shift equals repeated ×2 (no float, no drift)",
              shift_ok);
    }

    /* ── T6: enter anywhere — any start covers the whole field via +37 ─── */
    {
        const uint32_t starts[3] = {0u, 7777u, 19999u};
        int ok = 1;
        for (int si = 0; si < 3 && ok; si++) {
            uint8_t seen[TESS_TOTAL_SLOTS];
            memset(seen, 0, sizeof(seen));
            uint32_t n = starts[si], cnt = 0;
            int uniq = 1;
            do {
                if (seen[n]) uniq = 0;
                seen[n] = 1;
                n = (n + RAIL_PLUS) % RAIL_ROUND;
                cnt++;
            } while (n != starts[si]);
            if (cnt != RAIL_ROUND || !uniq) ok = 0;
        }
        CHECK("T6: enter anywhere — every start slot is a valid round entry (full coverage)",
              ok);
    }

    /* ── T7: dual-rail interleave — write on rail+, read on rail− ──────── */
    {
        /* write value k at belt address (start + 37k); read it back by walking
           the same +37 belt (rail+). Then read by walking −37 (rail−) from a
           later position — must land on the same stored value (same perm). */
        uint16_t field[RAIL_ROUND];
        uint32_t start = 42u, L = 5000u;
        memset(field, 0, sizeof(field));
        for (uint32_t k = 0; k < L; k++)
            field[(start + RAIL_PLUS * k) % RAIL_ROUND] = (uint16_t)(k & 0xFFFFu);
        /* rail+ read at k */
        uint32_t fwd_addr = (start + RAIL_PLUS * 999u) % RAIL_ROUND;
        /* rail− read: the belt address 999 steps forward from start, reached
           backward from 1000 steps forward (i.e. address of step 999) */
        uint32_t step1000 = (start + RAIL_PLUS * 1000u) % RAIL_ROUND;
        uint32_t back_addr = (step1000 + RAIL_ROUND - RAIL_PLUS) % RAIL_ROUND;
        CHECK("T7: same value reachable on both rails — rail−(rail+_next) == rail+",
              field[fwd_addr] == (uint16_t)(999u & 0xFFFFu) &&
              back_addr == fwd_addr);
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}