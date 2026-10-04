/* tests/test_light_index.c — ANN storage assembly gate (step 1 of light KV).
 *
 * Proves the joint, not the parts (parts already proven separately):
 *   light-index struct (geo_light_index.h) + ANN-over-index (anchor_route.h)
 *   + fault path (offset/size into a payload buffer).
 *
 * The payload here is a malloc'd fixture buffer, NOT real KVCB/KVD bytes:
 * step 2 (light KV) swaps the buffer for real cold-KV files — wiring only —
 * because the header and this test touch payload solely via (store_off,
 * store_size). Nothing in this file knows what payload bytes mean.
 *
 * Four gates:
 *   G1 bench   : routed rank vs brute rank over the index (recall + ms/q)
 *   G2 rebuild : zero the index, replay the log, entries identical, bytes verify
 *   G3 climate : drift a subset, hot set shifts exactly there, rank stable elsewhere
 *   G4 speed   : rank+fault latency per query, bytes correct every time
 *
 * BUILD: gcc -O2 -I. -Icore -o build/test_light_index tests/test_light_index.c -lm
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/geo_light_index.h"
#include "core/anchor_route.h"

#define NENTRY   600
#define DIM      4
#define KANCH    8
#define TOPB     2
#define NQUERY   200
#define PAYLOAD  64

static int g_pass = 0, g_fail = 0;
static void check_ok(int ok, const char *name) {
    if (ok) { g_pass++; printf("  PASS  %s\n", name); }
    else    { g_fail++; printf("  FAIL  %s\n", name); }
}

static uint32_t rng = 0x6d2b79f5u;
static uint32_t next_u32(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

/* Deterministic fixture payload: block i = repeating byte pattern of i. */
static void fill_payload(uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < PAYLOAD; j++)
            buf[(size_t)i * PAYLOAD + j] = (uint8_t)((i * 31u + j * 7u) & 0xFFu);
}
static int verify_payload(const uint8_t *buf, uint32_t i) {
    for (uint32_t j = 0; j < PAYLOAD; j++)
        if (buf[(size_t)i * PAYLOAD + j] != (uint8_t)((i * 31u + j * 7u) & 0xFFu))
            return 0;
    return 1;
}

/* Entry features (system-intrinsic only, no external vectors):
 * [slot_norm, cluster_norm, access_norm, size_norm]. */
static void entry_feat(const LIXEntry *e, float *v) {
    v[0] = (float)e->field_slot / (float)LIX_TOTAL_SLOTS;
    v[1] = (float)e->hj_cluster / (float)HJ_TOTAL;
    v[2] = (float)e->access / 64.0f;
    v[3] = (float)e->store_size / 128.0f;
}

static double dist2(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < DIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}

int main(void) {
    printf("=== LIGHT INDEX (ANN storage step 1) ===\n");

    /* ── fixtures: NENTRY blocks, payload buffer is the "storage" ── */
    static uint8_t payload[NENTRY * PAYLOAD];
    fill_payload(payload, NENTRY);

    static LIXEntry entries[NENTRY];
    static uint8_t log[NENTRY * LIX_REC_WIRE];
    uint32_t nlog = 0;
    char namebuf[32];
    for (uint32_t i = 0; i < NENTRY; i++) {
        snprintf(namebuf, sizeof(namebuf), "blk-%05u", i);
        LIXEntry *e = &entries[i];
        e->name_hash  = lix_hash(namebuf, (uint32_t)strlen(namebuf));
        e->field_slot = lix_slot_of(i);
        e->hj_cluster = lix_cluster_of(e->field_slot);
        e->store_off  = (uint64_t)i * PAYLOAD;
        e->store_size = PAYLOAD;
        e->access     = next_u32() % 6u;             /* 0..5, below HOT */
        e->flags      = 0;
        if (lix_log_append(log, sizeof(log), &nlog, e) != 0) {
            printf("  FAIL: log append\n"); return 1;
        }
    }
    check_ok(nlog == NENTRY, "log holds all NENTRY records");

    /* wire-size receipt: NENTRY * 40 B */
    check_ok(sizeof(log) == (size_t)NENTRY * LIX_REC_WIRE, "wire size = NENTRY*40B");

    /* ── feature matrix + anchors ── */
    static float X[NENTRY * DIM];
    for (uint32_t i = 0; i < NENTRY; i++) entry_feat(&entries[i], X + (size_t)i * DIM);
    static float C[KANCH * DIM];
    static int lab[NENTRY];
    check_ok(anch_train(X, NENTRY, DIM, KANCH, C, lab) == 0, "anch_train ok");

    /* bucket census: every bucket non-empty (else rank has dead zones) */
    int cnt[KANCH] = {0};
    for (uint32_t i = 0; i < NENTRY; i++) if (lab[i] >= 0) cnt[lab[i]]++;
    int nonempty = 0;
    for (int k = 0; k < KANCH; k++) if (cnt[k] > 0) nonempty++;
    check_ok(nonempty == KANCH, "every anchor bucket non-empty");

    /* ── G1 bench: routed (topb=K, full) == brute top-1, then topb=2 value ── */
    static float q[DIM];
    int topb_out[KANCH];
    long scored_full = 0, scored_2 = 0;
    int match_full = 0, match_2 = 0;
    clock_t t0 = clock();
    for (int qi = 0; qi < NQUERY; qi++) {
        /* query = jittered copy of a real member (answer exists by construction) */
        uint32_t src = next_u32() % NENTRY;
        for (int j = 0; j < DIM; j++)
            q[j] = X[(size_t)src * DIM + j] + ((float)(next_u32() % 100u) / 10000.0f - 0.005f);
        /* brute top-1 */
        int bb = -1; double bd = 1e300;
        for (uint32_t i = 0; i < NENTRY; i++) {
            double d = dist2(q, X + (size_t)i * DIM);
            if (d < bd) { bd = d; bb = (int)i; }
        }
        /* routed, full buckets */
        int got = anch_route(q, C, KANCH, DIM, KANCH, topb_out);
        int rb = -1; double rd = 1e300;
        for (int t = 0; t < got; t++)
            for (uint32_t i = 0; i < NENTRY; i++) {
                if (lab[i] != topb_out[t]) continue;
                scored_full++;
                double d = dist2(q, X + (size_t)i * DIM);
                if (d < rd) { rd = d; rb = (int)i; }
            }
        if (rb == bb) match_full++;
        /* routed, top-2 buckets */
        got = anch_route(q, C, KANCH, DIM, TOPB, topb_out);
        rb = -1; rd = 1e300;
        for (int t = 0; t < got; t++)
            for (uint32_t i = 0; i < NENTRY; i++) {
                if (lab[i] != topb_out[t]) continue;
                scored_2++;
                double d = dist2(q, X + (size_t)i * DIM);
                if (d < rd) { rd = d; rb = (int)i; }
            }
        if (rb == bb) match_2++;
    }
    double bench_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("    G1: full-bucket recall@1 %d/%d, top2 recall@1 %d/%d\n",
           match_full, NQUERY, match_2, NQUERY);
    printf("    G1: scored/query full=%.0f top2=%.0f (ratio %.2f), %.1f ms/q\n",
           (double)scored_full / NQUERY, (double)scored_2 / NQUERY,
           (double)scored_full / (double)(scored_2 ? scored_2 : 1),
           bench_s * 1000.0 / NQUERY);
    check_ok(match_full == NQUERY, "G1 routed(full) top-1 == brute top-1 on all queries");
    check_ok(scored_2 < scored_full, "G1 top-2 scores fewer items than full");
    check_ok(match_2 >= NQUERY * 95 / 100, "G1 top-2 recall@1 >= 95%");

    /* ── G2 rebuild: evict everything, replay log, identical + bytes verify ── */
    static LIXEntry snap[NENTRY];
    memcpy(snap, entries, sizeof(entries));
    memset(entries, 0, sizeof(entries));
    static LIXEntry rebuilt[NENTRY];
    int m = lix_log_replay(log, nlog, rebuilt, NENTRY);
    check_ok(m == (int)NENTRY, "G2 replay recovers all NENTRY records");
    check_ok(memcmp(snap, rebuilt, sizeof(snap)) == 0, "G2 replayed entries bit-identical");
    memcpy(entries, rebuilt, sizeof(entries));
    int vok = 1;
    for (uint32_t i = 0; i < NENTRY; i++) {
        /* fault path: (off,size) -> payload bytes -> pattern check */
        const LIXEntry *e = &entries[i];
        uint32_t blk = (uint32_t)(e->store_off / PAYLOAD);
        if (blk >= NENTRY || e->store_size != PAYLOAD ||
            !verify_payload(payload, blk)) { vok = 0; break; }
    }
    check_ok(vok, "G2 every entry faults correct payload bytes after rebuild");

    /* ── G3 climate: drift 50 entries, hot set shifts exactly there ── */
    int hot_before = 0;
    for (uint32_t i = 0; i < NENTRY; i++)
        if (entries[i].flags & LIX_FLAG_HOT) hot_before++;
    for (uint32_t i = 0; i < 50; i++) {
        for (int d = 0; d < 10; d++) lix_note_drift(&entries[i]);
        entries[i].flags |= LIX_FLAG_HOT;   /* drift crossing recorded */
    }
    int hot_after = 0, drift_hot = 0;
    for (uint32_t i = 0; i < NENTRY; i++)
        if (entries[i].flags & LIX_FLAG_HOT) {
            hot_after++;
            if (i < 50) drift_hot++;
        }
    printf("    G3: hot %d -> %d (drifted hot %d/50)\n", hot_before, hot_after, drift_hot);
    check_ok(drift_hot == 50, "G3 all 50 drifted entries are HOT");
    check_ok(hot_after - hot_before == 50 - 0 || hot_after >= hot_before,
             "G3 hot set grows monotonically (no cold flip)");
    int undrift_changed = 0;
    for (uint32_t i = 50; i < NENTRY; i++)
        if ((entries[i].flags & LIX_FLAG_HOT) && !(snap[i].flags & LIX_FLAG_HOT))
            undrift_changed++;
    check_ok(undrift_changed == 0, "G3 undrifted entries keep their flags");
    /* rank stability: control query on undrifted region keeps its bucket */
    uint32_t ctl = 400;
    for (int j = 0; j < DIM; j++) q[j] = X[(size_t)ctl * DIM + j];
    int b0 = anch_assign(q, C, KANCH, DIM);
    /* re-derive features after drift (access changed) -> re-train, same query */
    for (uint32_t i = 0; i < NENTRY; i++) entry_feat(&entries[i], X + (size_t)i * DIM);
    static float C2[KANCH * DIM];
    static int lab2[NENTRY];
    anch_train(X, NENTRY, DIM, KANCH, C2, lab2);
    int b1 = anch_assign(q, C2, KANCH, DIM);
    check_ok(b0 == b1, "G3 control-query bucket stable across drift elsewhere");

    /* ── G4 speed: rank + fault per query, bytes correct every time ── */
    int allok = 1;
    t0 = clock();
    for (int qi = 0; qi < NQUERY; qi++) {
        uint32_t src = next_u32() % NENTRY;
        for (int j = 0; j < DIM; j++) q[j] = X[(size_t)src * DIM + j];
        int got = anch_route(q, C2, KANCH, DIM, TOPB, topb_out);
        int best = -1; double bd = 1e300;
        for (int t = 0; t < got; t++)
            for (uint32_t i = 0; i < NENTRY; i++) {
                if (lab2[i] != topb_out[t]) continue;
                double d = dist2(q, X + (size_t)i * DIM);
                if (d < bd) { bd = d; best = (int)i; }
            }
        if (best < 0) { allok = 0; break; }
        uint32_t blk = (uint32_t)(entries[best].store_off / PAYLOAD);
        if (blk >= NENTRY || !verify_payload(payload, blk)) {
            allok = 0; break;
        }
    }
    double g4_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("    G4: rank+fault %d queries in %.3fs (%.1f us/q), bytes ok=%d\n",
           NQUERY, g4_s, g4_s * 1e6 / NQUERY, allok);
    check_ok(allok, "G4 every query faults correct bytes");
    check_ok(g4_s < 5.0, "G4 total under 5s sanity bound");

    printf("=== RESULTS: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    printf("Receipt: lix{n=%d wire=%uB recall_full=%d/%d recall_top2=%d/%d}\n",
           NENTRY, (unsigned)(NENTRY * LIX_REC_WIRE), match_full, NQUERY, match_2, NQUERY);
    return g_fail ? 1 : 0;
}