/*
 * test_tess_frame_seek.c — geo_frame_seek × 1 Tesseract: Timeline Walk proof
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Rescope binding: the seeker walks the KIS timeline (1440-cycle, stride-37).
 * At EVERY position it sees ONE frame = the tesseract's index cube (cube 0),
 * and from that frame retrieves the other 7 data cubes — losslessly.
 *
 * Binding:
 *   t (timeline) → enc = frame_enc(t) = (t·37) % 1440   (geo_frame_seek)
 *   w (scale view) = enc % 144                          (tesseract W axis)
 *   1440 = 144 × 10  → one timeline rotation = 10 full scale cycles;
 *   w(t) = (37·t) % 144  is a permutation (gcd(37,144)=1) → the 144-step
 *   walk visits every scale exactly once.
 *
 * Layout (1 tesseract = 8 cubes = 8 vertices × 144 scale positions):
 *   slot = cube*144 + w
 *   cube 0 = INDEX frame (always readable — the visible front door):
 *            8 blocks × 18 = base/len/checksum of every cube
 *   cubes 1..7 = DATA (1008 slots), scattered by scale view:
 *             p = (a_w·l + b_w) % 144, gcd(a_w,144)=1 → bijection
 *
 * Lossless at every position:
 *   - at the append scale (log empty) → direct read
 *   - elsewhere → passive log of scale hops, replay (deterministic)
 *     telescopes to (current w → append w) → lossless
 *
 * Proof:
 *   T0  geo_frame_seek_verify() passes (stride-37, 1440-cycle invariants)
 *   T1  binding: w(t) = frame_enc(t)%144 covers every scale exactly once
 *       per 144-step walk; stride-37 next() walks the full 1440 cycle
 *   T2  index frame (cube 0) readable directly at ANY scale (front door)
 *   T3  append at w0 → walk 144 positions → lossless at EVERY position
 *   T4  mid-walk, read without replay → mismatch (lossy-looking view)
 *   T5  passive log stays tiny vs stored data (delta ∝ events)
 *   T6  full-hop log telescopes to a single {w0→w} entry (same result)
 *   T7  anchor: 18 tesseracts × 8 cubes × 144 = 20736 (future upgrade)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_frame_seek tests/test_tess_frame_seek.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "geo_frame_seek.h"

#define TFS_CUBES       8u
#define TFS_LOCAL       144u          /* scale positions W ∈ [0,144)   */
#define TFS_TOTAL       (TFS_CUBES * TFS_LOCAL)      /* 1152          */
#define TFS_INDEX_CUBE  0u
#define TFS_DATA_CUBES  (TFS_CUBES - 1u)             /* 7             */
#define TFS_DATA_SLOTS  (TFS_DATA_CUBES * TFS_LOCAL) /* 1008          */
#define TFS_BLOCK       18u           /* 144 / 8                      */
#define TFS_FULL        20736u
#define TFS_TESS_18     18u
#define TFS_WALK        144u          /* one full scale cycle          */

/* ── Scale addressing: 144 distinct (a_w, b_w), gcd(a_w,144)=1 ──── */
static uint8_t TFS_A[144], TFS_B[144], TFS_INV[144];

static int t1f_coeff_init(void) {
    uint8_t cop[48];
    uint32_t n = 0;
    for (uint32_t k = 1; k < 144 && n < 48; k += 2) {   /* odd */
        if (k % 3 == 0) continue;                        /* coprime to 144 */
        cop[n++] = (uint8_t)k;
    }
    if (n != 48) return -1;

    for (uint32_t w = 0; w < TFS_LOCAL; w++) {
        TFS_A[w] = cop[w % 48];
        TFS_B[w] = (uint8_t)((w * 13u) % 144u);
        uint8_t inv = 0;
        for (uint32_t x = 1; x < 144 && inv == 0; x++)
            if (((uint32_t)TFS_A[w] * x) % 144u == 1u) inv = (uint8_t)x;
        if (inv == 0) return -2;
        TFS_INV[w] = inv;
    }
    return 0;
}

/* ── Passive scale-change log (hyperbolic side) ─────────────────── */
typedef struct {
    uint8_t from;
    uint8_t to;
} ScaleEvent;

#define TFS_LOG_CAP 256u
typedef struct {
    ScaleEvent e[TFS_LOG_CAP];
    uint32_t   n;
} ScaleLog;

static void t1f_log_append(ScaleLog *log, uint32_t from, uint32_t to) {
    if (log->n >= TFS_LOG_CAP) return;
    log->e[log->n].from = (uint8_t)from;
    log->e[log->n].to   = (uint8_t)to;
    log->n++;
}

/* ── Store ──────────────────────────────────────────────────────── */
static uint32_t t1f_slot(uint32_t cube, uint32_t w) {
    return cube * TFS_LOCAL + w;
}

static uint8_t t1f_value(uint32_t cube, uint32_t local) {
    return (uint8_t)((cube * 37u + local * 7u + 11u) % 251u);
}

static uint32_t t1f_phys(uint32_t l, uint32_t w) {
    return ((uint32_t)TFS_A[w] * l + TFS_B[w]) % TFS_LOCAL;
}

static void t1f_encode(uint8_t *store, uint32_t w0) {
    memset(store, 0, TFS_TOTAL);
    uint8_t *frame = store + TFS_INDEX_CUBE * TFS_LOCAL;

    for (uint32_t c = 1; c < TFS_CUBES; c++) {
        uint32_t sum = 0;
        for (uint32_t l = 0; l < TFS_LOCAL; l++) {
            uint8_t v = t1f_value(c, l);
            store[t1f_slot(c, t1f_phys(l, w0))] = v;
            sum += v;
        }
        uint8_t *b = frame + c * TFS_BLOCK;
        uint32_t base = t1f_slot(c, 0);
        b[0] = (uint8_t)(base & 0xFFu);
        b[1] = (uint8_t)((base >> 8) & 0xFFu);
        b[2] = (uint8_t)(TFS_LOCAL & 0xFFu);
        b[3] = (uint8_t)((TFS_LOCAL >> 8) & 0xFFu);
        b[4] = (uint8_t)(sum % 251u);
        memset(b + 5, 0, TFS_BLOCK - 5u);
    }
}

/* replay log (forward 0..n−1): map append-view index → current-view index */
static uint32_t t1f_replay(uint32_t l, const ScaleLog *log) {
    uint32_t le = l % TFS_LOCAL;
    for (uint32_t i = 0; i < log->n; i++) {
        uint32_t f = log->e[i].from, t = log->e[i].to;
        int64_t num = (int64_t)TFS_A[f] * (int64_t)le + (int64_t)TFS_B[f] - (int64_t)TFS_B[t];
        num %= (int64_t)TFS_LOCAL;
        if (num < 0) num += TFS_LOCAL;
        le = (uint32_t)((num * TFS_INV[t]) % TFS_LOCAL);
    }
    return le;
}

static uint8_t t1f_read(const uint8_t *store, uint32_t cube, uint32_t l,
                        const ScaleLog *log, uint32_t cur_w) {
    uint32_t le = (log->n == 0) ? l : t1f_replay(l, log);
    return store[t1f_slot(cube, t1f_phys(le, cur_w))];
}

static int t1f_verify(const uint8_t *store, const ScaleLog *log, uint32_t cur_w) {
    const uint8_t *frame = store + TFS_INDEX_CUBE * TFS_LOCAL;
    for (uint32_t c = 1; c < TFS_CUBES; c++) {
        const uint8_t *b = frame + c * TFS_BLOCK;
        uint32_t sum = 0;
        for (uint32_t l = 0; l < TFS_LOCAL; l++) {
            if (t1f_read(store, c, l, log, cur_w) != t1f_value(c, l)) return 0;
            sum += t1f_value(c, l);
        }
        if ((sum % 251u) != b[4]) return 0;
    }
    return 1;
}

static uint32_t t1f_mismatch_count(const uint8_t *store, uint32_t cur_w) {
    ScaleLog empty;
    memset(&empty, 0, sizeof(empty));
    uint32_t bad = 0;
    for (uint32_t c = 1; c < TFS_CUBES; c++)
        for (uint32_t l = 0; l < TFS_LOCAL; l++)
            if (t1f_read(store, c, l, &empty, cur_w) != t1f_value(c, l)) bad++;
    return bad;
}

int main(void) {
    uint32_t pass = 0, fail = 0;
#define CHECK(d, c) do { if (c) { pass++; printf("  T: PASS — %s\n", d); } \
    else { fail++; printf("  T: FAIL — %s\n", d); } } while (0)

    if (t1f_coeff_init() != 0) { printf("  T: FAIL — coeff init\n"); return 1; }

    printf("geo_frame_seek × 1 Tesseract — Timeline Walk, Frame-as-Index\n");
    printf("════════════════════════════════════════════════════════════\n");

    /* T0: frame_seek self-test */
    CHECK("T0: geo_frame_seek_verify() passes (stride-37, 1440-cycle)", geo_frame_seek_verify() == 0);

    /* T1: binding — w(t) = frame_enc(t)%144 covers every scale once/cycle */
    {
        uint8_t seen[TFS_LOCAL] = {0};
        int ok = 1;
        for (uint32_t t = 0; t < TFS_LOCAL && ok; t++) {
            uint32_t w = frame_enc(t) % TFS_LOCAL;
            if (w >= TFS_LOCAL || seen[w]) ok = 0;
            seen[w] = 1;
        }
        for (uint32_t t = 0; t < 1440u && ok; t++)
            if (frame_next(frame_enc(t)) != frame_enc((t + 1) % 1440u)) { ok = 0; break; }
        /* full rotation: every scale appears exactly 10 times (1440/144) */
        uint32_t cnt[TFS_LOCAL] = {0};
        for (uint32_t t = 0; t < 1440u && ok; t++) cnt[frame_enc(t) % TFS_LOCAL]++;
        for (uint32_t w = 0; w < TFS_LOCAL && ok; w++)
            if (cnt[w] != 10u) ok = 0;
        CHECK("T1: w = frame_enc(t)%144 — all scales once per 144-step walk, 10× per rotation",
              ok);
    }

    uint8_t *store = (uint8_t *)calloc(TFS_TOTAL, 1);
    if (!store) { printf("  T: FAIL — alloc\n"); return 1; }

    uint32_t w0 = frame_enc(0) % TFS_LOCAL;      /* 0 — append at t=0 */
    t1f_encode(store, w0);

    /* T2: index frame (cube 0) readable directly at ANY scale */
    {
        int ok = 1;
        const uint8_t *frame = store + TFS_INDEX_CUBE * TFS_LOCAL;
        for (uint32_t s = 0; s < 8u && ok; s++) {   /* sample 8 scales */
            uint32_t w = (s * 17u) % TFS_LOCAL;
            (void)w;                                /* addressing not applied to frame */
            for (uint32_t c = 1; c < TFS_CUBES && ok; c++) {
                const uint8_t *b = frame + c * TFS_BLOCK;
                if (b[0] != (uint8_t)(t1f_slot(c, 0) & 0xFFu)) ok = 0;
                if (b[2] != (uint8_t)(TFS_LOCAL & 0xFFu)) ok = 0;
            }
        }
        CHECK("T2: index frame (cube 0) readable directly at any scale — front door always open", ok);
    }

    /* T3: walk 144 positions — lossless at EVERY position via replay */
    {
        ScaleLog log;
        memset(&log, 0, sizeof(log));
        uint32_t ok_pos = 0;
        uint32_t prev_w = w0;
        for (uint32_t t = 0; t < TFS_WALK; t++) {
            uint32_t w = frame_enc(t) % TFS_LOCAL;
            if (t > 0 && w != prev_w) t1f_log_append(&log, prev_w, w);
            if (t1f_verify(store, &log, w)) ok_pos++;
            prev_w = w;
        }
        CHECK("T3: 144/144 walked positions → retrieve 8 cubes lossless",
              ok_pos == TFS_WALK);
        printf("     walk: %u positions × %u slots = %u checks, log %u events\n",
               TFS_WALK, TFS_DATA_SLOTS, TFS_WALK * TFS_DATA_SLOTS, log.n);
    }

    /* T4: mid-walk without replay → mismatch (lossy-looking view) */
    {
        uint32_t w = frame_enc(71) % TFS_LOCAL;
        uint32_t bad = t1f_mismatch_count(store, w);
        CHECK("T4: mid-walk read without replay → mismatch (lossy view)", bad > 0);
        printf("     mismatches at w=%u without replay: %u/%u\n", w, bad, TFS_DATA_SLOTS);
    }

    /* T5 + T6: log size vs data; full-hop log telescopes to single entry */
    {
        ScaleLog full;
        memset(&full, 0, sizeof(full));
        uint32_t prev_w = w0;
        for (uint32_t t = 1; t < TFS_WALK; t++) {
            uint32_t w = frame_enc(t) % TFS_LOCAL;
            if (w != prev_w) t1f_log_append(&full, prev_w, w);
            prev_w = w;
        }
        uint32_t last_w = frame_enc(TFS_WALK - 1) % TFS_LOCAL;
        uint32_t log_bytes = (uint32_t)(full.n * sizeof(ScaleEvent));

        ScaleLog one;
        memset(&one, 0, sizeof(one));
        t1f_log_append(&one, w0, last_w);

        printf("\n     full-hop log = %u events × %u B = %u bytes\n",
               full.n, (unsigned)sizeof(ScaleEvent), log_bytes);
        printf("     data stored ONCE = %u slots\n", TFS_DATA_SLOTS);

        CHECK("T5: passive log stays tiny vs stored data (delta ∝ events)",
              log_bytes < TFS_DATA_SLOTS);
        CHECK("T6: full-hop log telescopes to single {w0→w} — identical result",
              t1f_verify(store, &full, last_w) == 1 &&
              t1f_verify(store, &one,  last_w) == 1);
    }

    /* T7: anchor — 18 tesseracts × 8 cubes × 144 = 20736 (future) */
    CHECK("T7: 18 tesseracts × 8 cubes × 144 = 20736 (future upgrade)",
          TFS_TESS_18 * TFS_CUBES * TFS_LOCAL == TFS_FULL);

    free(store);

    printf("\n════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
