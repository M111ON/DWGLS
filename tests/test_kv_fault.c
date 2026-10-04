/* tests/test_kv_fault.c — fault path onto real KVCB/KVD files (step 2 wiring).
 *
 * The payload bytes are deterministic fixtures, but the CONTAINER is the
 * real cold-KV format: [24B KVCBFileHdr][blob] via kvcb_save/kvcb_load,
 * token suffix via kvcb_spill/kvcb_unspill (kv_cold_base.h). No llama
 * context is needed: save/load/spill/unspill never call llama_state_seq_*.
 * Only kvcb_hold/resume need a live context (not exercised here).
 *
 * The light index (geo_light_index.h) points at real file offsets
 * (store_off includes the 24B header); the fault path is real file IO
 * (fopen + fseek + fread per span). ANN rank (anchor_route.h) picks the
 * entry; the file yields the bytes. That is the whole step-2 joint.
 *
 * File: build/ann_kv_base.kvcb + build/ann_kv_delta.kvd (written by the
 * test itself — hermetic, no model file needed).
 *
 * Gates:
 *   F1 format  : save -> load roundtrip, hdr + blob identical
 *   F2 fault   : every entry's span fread from file == expected pattern
 *   F3 rank+fault: ANN top entry per query faults correct bytes from file
 *   F4 delta   : spill/unspill token suffix identical
 *   F5 rebuild : drop RAM index, replay log, fault again correct
 *
 * BUILD: gcc -O2 -I. -Icore -o build/test_kv_fault tests/test_kv_fault.c -lm
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/kv_cold_base.h"
#include "core/geo_light_index.h"
#include "core/anchor_route.h"

#define NBLK     32
#define BLKSZ    64
#define DIM      4
#define KANCH    4
#define TOPB     2
#define NQUERY   50

#define KVCB_PATH "build/ann_kv_base.kvcb"
#define KVD_PATH  "build/ann_kv_delta.kvd"

static int g_pass = 0, g_fail = 0;
static void check_ok(int ok, const char *name) {
    if (ok) { g_pass++; printf("  PASS  %s\n", name); }
    else    { g_fail++; printf("  FAIL  %s\n", name); }
}

/* Fixture block pattern (stands in for real llama state bytes). */
static void blk_fill(uint8_t *b, uint32_t i) {
    for (uint32_t j = 0; j < BLKSZ; j++)
        b[j] = (uint8_t)((i * 37u + j * 11u + 3u) & 0xFFu);
}
static int blk_check(const uint8_t *b, uint32_t i) {
    for (uint32_t j = 0; j < BLKSZ; j++)
        if (b[j] != (uint8_t)((i * 37u + j * 11u + 3u) & 0xFFu)) return 0;
    return 1;
}

static double dist2(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < DIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}
static void feat(const LIXEntry *e, float *v) {
    v[0] = (float)e->field_slot / (float)LIX_TOTAL_SLOTS;
    v[1] = (float)e->hj_cluster / (float)HJ_TOTAL;
    v[2] = (float)e->access / 64.0f;
    v[3] = (float)e->store_size / 128.0f;
}

/* Fault one span from the KVCB file. Returns 1 ok, 0 fail. */
static int fault_span(const char *path, uint64_t off, uint32_t size,
                      uint8_t *dst) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = 0;
    if (fseek(f, (long)off, SEEK_SET) == 0 &&
        fread(dst, 1, size, f) == size) ok = 1;
    fclose(f);
    return ok;
}

int main(void) {
    printf("=== KV FAULT (real KVCB/KVD files) ===\n");

    /* ── build the base blob: NBLK deterministic blocks ── */
    static uint8_t blob[NBLK * BLKSZ];
    for (uint32_t i = 0; i < NBLK; i++) blk_fill(blob + (size_t)i * BLKSZ, i);

    KVColdBase base;
    kvcb_init(&base);
    base.blob = (uint8_t *)malloc(sizeof(blob));
    if (!base.blob) { printf("  FAIL: malloc\n"); return 1; }
    memcpy(base.blob, blob, sizeof(blob));
    base.size = sizeof(blob);
    base.n_tokens = 128;
    base.have = 1;
    if (kvcb_save(&base, KVCB_PATH) != 0) {
        printf("  FAIL: kvcb_save (%s)\n", KVCB_PATH); return 1;
    }

    /* ── F1: save -> load roundtrip ── */
    KVColdBase re;
    kvcb_init(&re);
    int ld = kvcb_load(&re, KVCB_PATH);
    check_ok(ld == 0, "F1 kvcb_load ok");
    check_ok(re.have && re.size == base.size && re.n_tokens == 128,
             "F1 hdr intact (have/size/n_tokens)");
    check_ok(re.size == base.size && memcmp(re.blob, base.blob, base.size) == 0,
             "F1 blob bit-identical after roundtrip");
    kvcb_clear(&re);

    /* ── light index over the file (offsets include the 24B hdr) ── */
    static LIXEntry entries[NBLK];
    static uint8_t log[NBLK * LIX_REC_WIRE];
    uint32_t nlog = 0;
    char namebuf[32];
    for (uint32_t i = 0; i < NBLK; i++) {
        snprintf(namebuf, sizeof(namebuf), "kvblk-%05u", i);
        LIXEntry *e = &entries[i];
        e->name_hash  = lix_hash(namebuf, (uint32_t)strlen(namebuf));
        e->field_slot = lix_slot_of(i * 101u + 7u);
        e->hj_cluster = lix_cluster_of(e->field_slot);
        e->store_off  = (uint64_t)sizeof(KVCBFileHdr) + (uint64_t)i * BLKSZ;
        e->store_size = BLKSZ;
        e->access     = i % 5u;
        e->flags      = 0;
        if (lix_log_append(log, sizeof(log), &nlog, e) != 0) {
            printf("  FAIL: log append\n"); return 1;
        }
    }

    /* ── F2: fault every span from the real file ── */
    static uint8_t span[BLKSZ];
    int fok = 1;
    for (uint32_t i = 0; i < NBLK; i++) {
        if (!fault_span(KVCB_PATH, entries[i].store_off, entries[i].store_size, span) ||
            !blk_check(span, i)) { fok = 0; break; }
    }
    check_ok(fok, "F2 all 32 spans fault correct bytes from KVCB file");

    /* ── anchors over the index ── */
    static float X[NBLK * DIM];
    for (uint32_t i = 0; i < NBLK; i++) feat(&entries[i], X + (size_t)i * DIM);
    static float C[KANCH * DIM];
    static int lab[NBLK];
    check_ok(anch_train(X, NBLK, DIM, KANCH, C, lab) == 0, "anchors train ok");

    /* ── F3: rank + fault from file ── */
    static float q[DIM];
    static int buckets[KANCH];
    uint32_t qrng = 0x77aa55ul;
    int rok = 1;
    for (int qi = 0; qi < NQUERY && rok; qi++) {
        qrng ^= qrng << 13; qrng ^= qrng >> 17; qrng ^= qrng << 5;
        uint32_t src = qrng % NBLK;
        for (int j = 0; j < DIM; j++) q[j] = X[(size_t)src * DIM + j];
        int got = anch_route(q, C, KANCH, DIM, TOPB, buckets);
        int best = -1; double bd = 1e300;
        for (int t = 0; t < got; t++)
            for (uint32_t i = 0; i < NBLK; i++) {
                if (lab[i] != buckets[t]) continue;
                double d = dist2(q, X + (size_t)i * DIM);
                if (d < bd) { bd = d; best = (int)i; }
            }
        if (best < 0) { rok = 0; break; }
        if (!fault_span(KVCB_PATH, entries[best].store_off,
                        entries[best].store_size, span) ||
            !blk_check(span, (uint32_t)best)) rok = 0;
    }
    check_ok(rok, "F3 all 50 rank+fault queries yield correct file bytes");

    /* ── F4: token-suffix delta spill/unspill ── */
    static int32_t dtoks[16];
    for (int i = 0; i < 16; i++) dtoks[i] = 1000 + i * 37;
    int sp = kvcb_spill(dtoks, 16, 128, KVD_PATH);
    check_ok(sp == 0, "F4 kvcb_spill ok");
    int32_t *back = NULL;
    uint32_t nback = 0;
    int32_t base_n = 0;
    int un = kvcb_unspill(&back, &nback, &base_n, KVD_PATH);
    check_ok(un == 0 && nback == 16 && base_n == 128, "F4 kvcb_unspill count+base");
    check_ok(back && memcmp(back, dtoks, sizeof(dtoks)) == 0,
             "F4 suffix tokens identical");
    free(back);

    /* ── F5: drop RAM index, replay log, fault again ── */
    static LIXEntry snap[NBLK];
    memcpy(snap, entries, sizeof(entries));
    memset(entries, 0, sizeof(entries));
    static LIXEntry rebuilt[NBLK];
    int m = lix_log_replay(log, nlog, rebuilt, NBLK);
    check_ok(m == (int)NBLK, "F5 replay recovers all records");
    check_ok(memcmp(snap, rebuilt, sizeof(snap)) == 0, "F5 replayed bit-identical");
    memcpy(entries, rebuilt, sizeof(entries));
    fok = 1;
    for (uint32_t i = 0; i < NBLK; i++) {
        if (!fault_span(KVCB_PATH, entries[i].store_off, entries[i].store_size, span) ||
            !blk_check(span, i)) { fok = 0; break; }
    }
    check_ok(fok, "F5 post-rebuild faults still correct");

    kvcb_clear(&base);
    printf("=== RESULTS: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    printf("Receipt: kvfault{n=%d f1f2f3f4f5, files=" KVCB_PATH " + " KVD_PATH "}\n", NBLK);
    return g_fail ? 1 : 0;
}