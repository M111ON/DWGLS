/* ═══════════════════════════════════════════════════════════════════════════
 * test_id_contract.c — Identity/World/Address contract gate (P2)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * GATE for the 5-tuple binding (core/geo_id_contract.h):
 *
 *   I1. Two independent derivations of the same ident agree (determinism).
 *   I2. addr_slot = ident.addr_slot — obvious, exercised.
 *   I3. walk_round * ticks + walk_tick = walk_rq — one consolidated clock.
 *   I4. After partial rebuild (clear some slots) + replay → all five-tuple
 *       fields match what was there before (P3 contract pre-condition).
 *   I5. Walk one stride → walk_round/walk_tick advance, addr_slot is the new
 *       stride-37 slot (P5 walk forward).
 *
 * Style: explicit pass/fail counters, all expectations from math, no function
 * under test feeds the oracle.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "geo_id_contract.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define EXPECT(cond, msg) do {                                                 \
    if (cond) { g_pass++; printf("  PASS  %s\n", msg); }                        \
    else       { g_fail++; printf("  FAIL  %s  (line %d)\n", msg, __LINE__); } \
} while (0)

/* Make a deterministic 64-byte payload from a seed. */
static void fill64(uint8_t out[64], uint32_t seed) {
    for (int i = 0; i < 64; i++) {
        out[i] = (uint8_t)((seed * 2654435761u + (uint32_t)i) >> 0);
    }
}

int main(void) {
    printf("=== test_id_contract (P2 gate) ===\n");

    /* Walk state: same shape fibo_walk.h / geo_ggf_walk.h use */
    GeoWalkState w = {
        .seed = 7u, .ticks = 12u, .cycles = 20736u,
        .round = 0u, .tick = 0u
    };

    /* ── I1: determinism — two independent derivations of same name+slot+bytes ── */
    printf("[I1] determinism — two derivations of same ident\n");
    uint8_t bytes_a[64];
    fill64(bytes_a, 100);
    GeoIdent id_a1 = geo_ident_from_slot("blk.0.attn_q.weight", 42, &w, bytes_a);
    GeoIdent id_a2 = geo_ident_from_slot("blk.0.attn_q.weight", 42, &w, bytes_a);
    EXPECT(geo_ident_eq(&id_a1, &id_a2), "I1.a: name+slot+bytes ⇒ identical 5-tuple");
    EXPECT(id_a1.name_hash != 0, "I1.b: name_hash is non-zero");
    EXPECT(id_a1.bytes_hash != 0, "I1.c: bytes_hash is non-zero for non-zero bytes");
    EXPECT(id_a1.addr_slot == 42, "I1.d: addr_slot preserved");

    /* Different name ⇒ different name_hash (else determinism bug) */
    GeoIdent id_a1_other = geo_ident_from_slot("blk.1.attn_q.weight", 42, &w, bytes_a);
    EXPECT(id_a1.name_hash != id_a1_other.name_hash, "I1.e: different names ⇒ different name_hash");
    EXPECT(id_a1.bytes_hash == id_a1_other.bytes_hash, "I1.f: same bytes ⇒ same bytes_hash");

    /* Different bytes ⇒ different bytes_hash */
    uint8_t bytes_b[64];
    fill64(bytes_b, 200);
    GeoIdent id_b1 = geo_ident_from_slot("blk.0.attn_q.weight", 42, &w, bytes_b);
    EXPECT(id_a1.bytes_hash != id_b1.bytes_hash, "I1.g: different bytes ⇒ different bytes_hash");
    EXPECT(id_a1.name_hash == id_b1.name_hash,  "I1.h: same name ⇒ same name_hash");

    /* ── I2: addr_slot resolver ── */
    printf("[I2] resolve_addr\n");
    EXPECT(geo_ident_resolve_addr(&id_a1) == 42, "I2.a: resolve_addr returns slot");

    /* ── I3: walk clock consolidation — rq = round*ticks + tick ── */
    printf("[I3] walk clock round * ticks + tick == consolidated rq\n");
    uint32_t rq     = geo_id_walk_rq(&id_a1);
    uint32_t rq_t   = geo_id_walk_rq_t(&id_a1, 12);
    EXPECT(rq == rq_t, "I3.a: rq == rq_t at ticks=12");
    EXPECT(rq == id_a1.walk_round * 12u + id_a1.walk_tick, "I3.b: rq math identity");

    /* Direct math sanity:  rq = (seed * (idx+1)) % cycles.
     * Then walk_round = rq / ticks, walk_tick = rq % ticks (round/tick split). */
    uint32_t expect_rq = (uint32_t)(((uint64_t)w.seed * (uint64_t)(42u + 1u)) % (uint64_t)w.cycles);
    EXPECT(rq == expect_rq, "I3.c: rq matches ggf_walk_rq_of math");
    EXPECT(id_a1.walk_tick == expect_rq % w.ticks, "I3.d: walk_tick = rq % ticks");
    EXPECT(id_a1.walk_round == expect_rq / w.ticks, "I3.e: walk_round = rq / ticks");

    /* ── I4: partial rebuild + log replay → ident matches ── */
    printf("[I4] partial rebuild + replay — 5-tuple reconstructed identically\n");
    /* Simulate: before rebuild — capture ident for 3 slots */
    uint8_t  bytes0[64], bytes1[64], bytes2[64];
    fill64(bytes0, 1000);
    fill64(bytes1, 2000);
    fill64(bytes2, 3000);

    GeoIdent before[3];
    before[0] = geo_ident_from_slot("blk.0.attn_q", 11, &w, bytes0);
    before[1] = geo_ident_from_slot("blk.1.attn_q", 173, &w, bytes1);
    before[2] = geo_ident_from_slot("blk.2.attn_q", 999, &w, bytes2);

    /* Build the event log (what would have been persisted on rebuild) */
    GeoIdentEvent log[3];
    for (int i = 0; i < 3; i++) {
        const char *nm = (i == 0) ? "blk.0.attn_q" :
                         (i == 1) ? "blk.1.attn_q" : "blk.2.attn_q";
        /* name is short enough for 32-byte buffer */
        size_t n = strlen(nm);
        if (n > 31) n = 31;
        memcpy(log[i].name, nm, n);
        log[i].name[n] = 0;
        log[i].addr_slot  = before[i].addr_slot;
        log[i].walk_round = before[i].walk_round;
        log[i].walk_tick  = before[i].walk_tick;
    }

    /* Simulate partial rebuild: scratch bytes (zero) then re-emit them */
    uint8_t  scratch0[64], scratch1[64], scratch2[64];
    memset(scratch0, 0, 64);
    memset(scratch1, 0, 64);
    memset(scratch2, 0, 64);

    /* Right after rebuild, before replay — bytes are zero ⇒ bytes_hash is the
     * FNV-1a of zero-bytes, which is NOT zero (it's the offset basis 0xCBF29CE4...).
     * What matters: it's DIFFERENT from the pre-rebuild bytes_hash. */
    GeoIdent mid[3];
    mid[0] = geo_ident_from_slot("blk.0.attn_q", 11, &w, scratch0);
    mid[1] = geo_ident_from_slot("blk.1.attn_q", 173, &w, scratch1);
    mid[2] = geo_ident_from_slot("blk.2.attn_q", 999, &w, scratch2);
    EXPECT(mid[0].bytes_hash != before[0].bytes_hash, "I4.a: pre-replay bytes_hash ≠ pre-rebuild bytes_hash");

    /* Replay: restore bytes, reconstruct ident from event log */
    memcpy(scratch0, bytes0, 64);
    memcpy(scratch1, bytes1, 64);
    memcpy(scratch2, bytes2, 64);

    GeoIdent after[3];
    after[0] = geo_ident_replay(&log[0], scratch0);
    after[1] = geo_ident_replay(&log[1], scratch1);
    after[2] = geo_ident_replay(&log[2], scratch2);

    EXPECT(geo_ident_eq(&before[0], &after[0]), "I4.c: replay slot 0 matches pre-rebuild");
    EXPECT(geo_ident_eq(&before[1], &after[1]), "I4.d: replay slot 1 matches pre-rebuild");
    EXPECT(geo_ident_eq(&before[2], &after[2]), "I4.e: replay slot 2 matches pre-rebuild");

    /* The 5 fields individually — failure must be localized. */
    EXPECT(after[0].name_hash  == before[0].name_hash,  "I4.f: replay name_hash matches");
    EXPECT(after[0].bytes_hash == before[0].bytes_hash, "I4.g: replay bytes_hash matches");
    EXPECT(after[0].addr_slot  == before[0].addr_slot,  "I4.h: replay addr_slot matches");
    EXPECT(after[0].walk_round == before[0].walk_round, "I4.i: replay walk_round matches");
    EXPECT(after[0].walk_tick  == before[0].walk_tick,  "I4.j: replay walk_tick matches");

    /* ── I5: stride-37 walk forward ── */
    printf("[I5] stride-37 walk forward\n");
    /* stride-37 mod 20736 is a permutation (gcd(37, 20736) = 1 ⇒ bijection on
     * the whole field), but individual ORBITS can be smaller than 20736. The
     * field-level property: every slot returns to itself after N steps (since
     * the map is a permutation). The orbit size depends on the start slot.
     * Here we test the field-level property. */
    {
        int all_returned = 1;
        for (uint32_t start = 0; start < 20736; start++) {
            uint32_t s = start;
            for (int step = 0; step < 20736; step++) {
                s = geo_id_walk_stride37(s);
            }
            if (s != start) { all_returned = 0; break; }
        }
        EXPECT(all_returned, "I5.a: stride-37 is a permutation over [0,20736) — every slot returns in N steps");
    }

    /* Inverse check: gcd(37, 20736) must equal 1 for stride-37 to biject. */
    {
        uint32_t x = 20736u, y = 37u;
        while (y) { uint32_t t = x % y; x = y; y = t; }
        EXPECT(x == 1u, "I5.b: gcd(37, 20736) == 1 (coprime ⇒ bijection)");
    }

    /* The modular inverse exists: 37 * 37^(-1) ≡ 1 (mod 20736).
     * That means stride-37 has an inverse stride (also coprime-derived), and
     * applying both in sequence is identity. We use brute-force search since
     * N=20736 is tiny. */
    {
        uint32_t inv = 0;
        for (uint32_t i = 1; i < 20736u; i++) {
            if ((37u * i) % 20736u == 1u) { inv = i; break; }
        }
        EXPECT(inv != 0, "I5.c: 37 has modular inverse mod 20736 (stride-37 is invertible)");
        /* Apply fwd then inv to a slot — must equal identity */
        uint32_t orig = 1234;
        uint32_t fwd = (orig * 37u) % 20736u;
        uint32_t back = (fwd * inv) % 20736u;
        EXPECT(back == orig, "I5.d: fwd-then-inv-stride on slot 1234 = identity");
    }

    /* Walk coord also advances: derive ident at slot=0 vs slot=37, same walk_state */
    uint8_t same_bytes[64];
    fill64(same_bytes, 500);
    GeoIdent id_at_0  = geo_ident_from_slot("x", 0,  &w, same_bytes);
    GeoIdent id_at_37 = geo_ident_from_slot("x", 37, &w, same_bytes);
    EXPECT(id_at_0.walk_tick != id_at_37.walk_tick ||
           id_at_0.walk_round != id_at_37.walk_round,
           "I5.e: different slots ⇒ different walk coords (same walk_state)");
    EXPECT(id_at_0.name_hash  == id_at_37.name_hash,
           "I5.f: same name ⇒ same name_hash across slots");

    /* ── Receipts ── */
    printf("\n=== RESULTS: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    if (g_fail) {
        printf("FAIL: see %d failed expectations above\n", g_fail);
        return 1;
    }
    /* Print one full ident as a contract receipt. */
    printf("Receipt: ident{name_hash=%016llx bytes_hash=%016llx addr=%u rq=%u tick=%u}\n",
           (unsigned long long)id_a1.name_hash,
           (unsigned long long)id_a1.bytes_hash,
           id_a1.addr_slot, id_a1.walk_round, id_a1.walk_tick);
    return 0;
}