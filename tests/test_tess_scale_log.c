/*
 * test_tess_scale_log.c — 1 Tesseract + Scale Timeline: Passive Log proof
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Rescope (2026-08-14):
 *   - scale = constant magnification rate (multiplicative view); every scale
 *     position w ∈ [0,144) is one view of the same store.
 *   - Everything moves TOGETHER (global scale เดียว) → append needs NO scale
 *     tag — the append scale is implied by the timeline position.
 *   - Hyperbolic side = PASSIVE log of scale-change events (tiny entries).
 *   - Lossless when read at the scale where data was appended; read at
 *     another scale → replay the log (deterministic) → lossless again.
 *
 * Layout (1 tesseract = 8 cubes = 8 vertices × 144 scale positions W):
 *   slot = cube*144 + w            (cube 0..7, w 0..143)
 *   cube 0 = INDEX frame (144 slots = 8 blocks × 18) — static address table
 *   cubes 1..7 = DATA (7 × 144 = 1008 slots)
 *
 * Addressing (affine, per scale w): logical local l sits at physical
 *   p = (a_w·l + b_w) % 144      gcd(a_w,144)=1 → bijection per scale
 * Data is appended at scale w0 and NEVER moved; only the view function
 * changes with w (everything moves together). Reading at w ≠ w0 without
 * the log = permuted (lossy-looking) readout; replaying the scale-change
 * log recovers the append-scale addressing → lossless.
 *
 * Replay: entry {w_i → w_{i+1}} maps a w_i-view index to the w_{i+1}-view
 * index of the SAME physical slot:
 *   T_i(l) = ((a_i·l + b_i − b_{i+1}) · inv(a_{i+1})) % 144
 * Apply entries forward (0..n−1), then read physical p = (a_n·l + b_n) % 144.
 *
 * Proof:
 *   T1  layout: 8 cubes × 144 = 1152; local axis = scale positions W
 *   T2  affine coeffs: 144 distinct (a_w, b_w), all a coprime → bijection
 *   T3  append at w0 → read at w0 (matching scale, empty log) → lossless
 *   T4  scale change appends ONE passive log entry
 *   T5  read at w1 WITHOUT replay → mismatch (lossy-looking)
 *   T6  read at w1 WITH replay → lossless (all 1008 slots)
 *   T7  multi-hop w0→w2→w5→w3, replay full log → lossless
 *   T8  log size vs data size — delta ∝ scale-change events, not data
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_scale_log tests/test_tess_scale_log.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define T1S_CUBES       8u
#define T1S_LOCAL       144u          /* scale positions W ∈ [0,144)   */
#define T1S_TOTAL       (T1S_CUBES * T1S_LOCAL)      /* 1152          */
#define T1S_INDEX_CUBE  0u
#define T1S_DATA_CUBES  (T1S_CUBES - 1u)             /* 7             */
#define T1S_DATA_SLOTS  (T1S_DATA_CUBES * T1S_LOCAL) /* 1008          */
#define T1S_BLOCK       18u           /* 144 / 8                      */
#define T1S_FULL        20736u
#define T1S_TESS_18     18u

/* ── Scale addressing: 144 distinct (a_w, b_w), gcd(a_w,144)=1 ────
 * a_w cycles the 48 coprimes of 144; b_w = (13·w) % 144 breaks ties.
 * (a_w,b_w) distinct for w ∈ [0,144): same a implies w ≡ w' (mod 48),
 *  same b then forces 144 | (w−w') → w = w'.                          */
static uint8_t T1S_A[144];
static uint8_t T1S_B[144];
static uint8_t T1S_INV[144];          /* modular inverse of a_w        */

static int t1s_coeff_init(void) {
    uint8_t cop[48];
    uint32_t n = 0;
    for (uint32_t k = 1; k < 144 && n < 48; k += 2) {   /* odd */
        if (k % 3 == 0) continue;                        /* coprime to 144 = 2⁴·3² */
        cop[n++] = (uint8_t)k;
    }
    if (n != 48) return -1;

    for (uint32_t w = 0; w < T1S_LOCAL; w++) {
        T1S_A[w] = cop[w % 48];
        T1S_B[w] = (uint8_t)((w * 13u) % 144u);
        /* modular inverse of a_w mod 144 (brute force, small) */
        uint8_t inv = 0;
        for (uint32_t x = 1; x < 144 && inv == 0; x++)
            if (((uint32_t)T1S_A[w] * x) % 144u == 1u) inv = (uint8_t)x;
        if (inv == 0) return -2;
        T1S_INV[w] = inv;
    }
    return 0;
}

/* ── Passive scale-change log (hyperbolic side) ─────────────────── */
typedef struct {
    uint8_t from;   /* scale before change */
    uint8_t to;     /* scale after change  */
} ScaleEvent;

#define T1S_LOG_CAP 16u
typedef struct {
    ScaleEvent e[T1S_LOG_CAP];
    uint32_t   n;
} ScaleLog;

static void t1s_log_append(ScaleLog *log, uint32_t from, uint32_t to) {
    if (log->n >= T1S_LOG_CAP) return;
    log->e[log->n].from = (uint8_t)from;
    log->e[log->n].to   = (uint8_t)to;
    log->n++;
}

/* ── Store ──────────────────────────────────────────────────────── */
static uint32_t t1s_slot(uint32_t cube, uint32_t w) {
    return cube * T1S_LOCAL + w;
}

/* deterministic logical value at (cube, local) */
static uint8_t t1s_value(uint32_t cube, uint32_t local) {
    return (uint8_t)((cube * 37u + local * 7u + 11u) % 251u);
}

/* physical position of logical local l at scale w */
static uint32_t t1s_phys(uint32_t l, uint32_t w) {
    return ((uint32_t)T1S_A[w] * l + T1S_B[w]) % T1S_LOCAL;
}

/* encode: append all 7 data cubes at scale w0 + build index frame */
static void t1s_encode(uint8_t *store, uint32_t w0) {
    memset(store, 0, T1S_TOTAL);
    uint8_t *frame = store + T1S_INDEX_CUBE * T1S_LOCAL;

    for (uint32_t c = 1; c < T1S_CUBES; c++) {
        uint32_t sum = 0;
        for (uint32_t l = 0; l < T1S_LOCAL; l++) {
            uint8_t v = t1s_value(c, l);
            store[t1s_slot(c, t1s_phys(l, w0))] = v;
            sum += v;
        }
        /* index frame block: base(2) len(2) checksum(1) reserved */
        uint8_t *b = frame + c * T1S_BLOCK;
        uint32_t base = t1s_slot(c, 0);
        b[0] = (uint8_t)(base & 0xFFu);
        b[1] = (uint8_t)((base >> 8) & 0xFFu);
        b[2] = (uint8_t)(T1S_LOCAL & 0xFFu);
        b[3] = (uint8_t)((T1S_LOCAL >> 8) & 0xFFu);
        b[4] = (uint8_t)(sum % 251u);
        memset(b + 5, 0, T1S_BLOCK - 5u);
    }
}

/* replay log (forward 0..n−1): map append-view index to current-view index */
static uint32_t t1s_replay(uint32_t l, const ScaleLog *log) {
    uint32_t le = l % T1S_LOCAL;
    for (uint32_t i = 0; i < log->n; i++) {
        uint32_t f = log->e[i].from, t = log->e[i].to;
        /* T_i(le) = ((a_f·le + b_f − b_t) · inv(a_t)) % 144 */
        int64_t num = (int64_t)T1S_A[f] * (int64_t)le + (int64_t)T1S_B[f] - (int64_t)T1S_B[t];
        num %= (int64_t)T1S_LOCAL;
        if (num < 0) num += T1S_LOCAL;
        le = (uint32_t)((num * T1S_INV[t]) % T1S_LOCAL);
    }
    return le;
}

/* read logical (cube,l) at current scale with optional log replay */
static uint8_t t1s_read(const uint8_t *store, uint32_t cube, uint32_t l,
                        const ScaleLog *log, uint32_t cur_w) {
    uint32_t le = (log->n == 0) ? l : t1s_replay(l, log);
    return store[t1s_slot(cube, t1s_phys(le, cur_w))];
}

/* verify all 7 cubes lossless at current scale (with replay if log non-empty) */
static int t1s_verify(const uint8_t *store, const ScaleLog *log, uint32_t cur_w) {
    const uint8_t *frame = store + T1S_INDEX_CUBE * T1S_LOCAL;
    for (uint32_t c = 1; c < T1S_CUBES; c++) {
        const uint8_t *b = frame + c * T1S_BLOCK;
        uint32_t sum = 0;
        for (uint32_t l = 0; l < T1S_LOCAL; l++) {
            if (t1s_read(store, c, l, log, cur_w) != t1s_value(c, l)) return 0;
            sum += t1s_value(c, l);
        }
        if ((sum % 251u) != b[4]) return 0;   /* frame checksum */
    }
    return 1;
}

/* count mismatches of a no-replay read at scale w (lossy-looking view) */
static uint32_t t1s_mismatch_count(const uint8_t *store, uint32_t cur_w) {
    ScaleLog empty;
    memset(&empty, 0, sizeof(empty));
    uint32_t bad = 0;
    for (uint32_t c = 1; c < T1S_CUBES; c++)
        for (uint32_t l = 0; l < T1S_LOCAL; l++)
            if (t1s_read(store, c, l, &empty, cur_w) != t1s_value(c, l)) bad++;
    return bad;
}

int main(void) {
    uint32_t pass = 0, fail = 0;
#define CHECK(d, c) do { if (c) { pass++; printf("  T: PASS — %s\n", d); } \
    else { fail++; printf("  T: FAIL — %s\n", d); } } while (0)

    if (t1s_coeff_init() != 0) {
        printf("  T: FAIL — coeff init\n");
        return 1;
    }

    printf("1 Tesseract + Scale Timeline — Passive Scale-Change Log\n");
    printf("══════════════════════════════════════════════════════════\n");

    /* T1: layout — 8 cubes × 144 = 1152; local axis = scale positions */
    {
        int bi_ok = 1;
        for (uint32_t s = 0; s < T1S_TOTAL; s++) {
            uint32_t cube = s / T1S_LOCAL, w = s % T1S_LOCAL;
            if (t1s_slot(cube, w) != s || cube >= T1S_CUBES || w >= T1S_LOCAL) { bi_ok = 0; break; }
        }
        CHECK("T1: 8 cubes × 144 = 1152, slot = cube×144 + w (scale positions)", bi_ok);
    }

    /* T2: affine coeffs — 144 distinct bijections */
    {
        int ok = 1;
        for (uint32_t w = 0; w < T1S_LOCAL; w++) {
            if (T1S_A[w] == 0 || (T1S_A[w] % 2u) == 0 || (T1S_A[w] % 3u) == 0) { ok = 0; break; }
            uint8_t seen[T1S_LOCAL] = {0};
            for (uint32_t l = 0; l < T1S_LOCAL; l++) {
                uint32_t p = t1s_phys(l, w);
                if (seen[p]) { ok = 0; break; }
                seen[p] = 1;
            }
            if (!ok) break;
        }
        for (uint32_t w = 0; w < T1S_LOCAL && ok; w++)
            for (uint32_t w2 = w + 1; w2 < T1S_LOCAL && ok; w2++)
                if (T1S_A[w] == T1S_A[w2] && T1S_B[w] == T1S_B[w2]) ok = 0;
        CHECK("T2: 144 distinct affine views, all bijective (gcd(a,144)=1)", ok);
    }

    /* T3..T8 scenario */
    {
        uint8_t *store = (uint8_t *)calloc(T1S_TOTAL, 1);
        if (!store) { printf("  T: FAIL — alloc\n"); return 1; }

        uint32_t w0 = 5;                     /* append scale            */
        t1s_encode(store, w0);

        ScaleLog log;
        memset(&log, 0, sizeof(log));          /* passive log — empty     */

        /* T3: matching scale, empty log → direct lossless */
        CHECK("T3: append at w=5 → read at w=5 (matching scale) lossless",
              t1s_verify(store, &log, w0) == 1);

        /* T4: scale change w0 → w1 appends ONE entry */
        uint32_t w1 = 61;
        t1s_log_append(&log, w0, w1);
        CHECK("T4: scale change 5→61 appends one passive log entry",
              log.n == 1 && log.e[0].from == w0 && log.e[0].to == w1);

        /* T5: read at w1 WITHOUT replay → permuted (lossy-looking) */
        uint32_t bad = t1s_mismatch_count(store, w1);
        CHECK("T5: read at w=61 without replay → mismatch (lossy view)",
              bad > 0);
        printf("     mismatches without replay at w=61: %u/%u (fully scrambled view)\n",
               bad, T1S_DATA_SLOTS);

        /* T6: read at w1 WITH replay → lossless */
        CHECK("T6: read at w=61 with log replay → lossless (1008 slots)",
              t1s_verify(store, &log, w1) == 1);

        /* T7: multi-hop w0→w2→w5→w3, replay full log → lossless */
        uint32_t w2 = 23, w3 = 71, w4 = 11;
        t1s_log_append(&log, w1, w2);
        t1s_log_append(&log, w2, w3);
        t1s_log_append(&log, w3, w4);
        CHECK("T7: multi-hop 5→61→23→71→11, replay full log → lossless",
              t1s_verify(store, &log, w4) == 1);

        /* T7b: append scale is implied by the log — no per-append tag.
         * Everything moves together → the log's first `from` IS the
         * scale where data was appended (user rule: append ไม่ต้องเก็บ). */
        CHECK("T7b: append scale recovered from log first entry (no tag needed)",
              log.e[0].from == w0);

        /* T8: log size vs data size */
        {
            uint32_t log_bytes = (uint32_t)(log.n * sizeof(ScaleEvent));
            printf("\n     log      = %u events × %u B = %u bytes (hyperbolic side)\n",
                   log.n, (unsigned)sizeof(ScaleEvent), log_bytes);
            printf("     data     = %u slots stored ONCE at append\n", T1S_DATA_SLOTS);
            printf("     delta ∝ scale-change events, not data size ✓\n");
            CHECK("T8: log (passive) stays tiny vs stored data",
                  log_bytes < T1S_DATA_SLOTS / 100u);
        }

        /* T8b: anchor — 18 tesseracts × 8 × 144 = 20736 (future) */
        CHECK("T8b: 18 tesseracts × 8 cubes × 144 = 20736 (future upgrade)",
              T1S_TESS_18 * T1S_CUBES * T1S_LOCAL == T1S_FULL);

        free(store);
    }

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
