/*
 * test_tess_magnify.c — Magnify Glass (20736÷4) × 1 Tesseract proof
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Rescope: the seeker reads the timeline through a MAGNIFY GLASS at the
 * center of the window:
 *   - timeline space 20736 ÷ 4 = 5184 per quadrant
 *   - the middle of the window is the magnify glass
 *   - magnification rate is INVERTED on the opposite side (antipode)
 *   - a small +offset shifts the glass center ("ต้อง+offset นิดหน่อย")
 *   - the opposite side is the hyperbolic side (compressed, delta log)
 *
 * 1-tesseract projection (scale axis w ∈ [0,144), 4 quadrants × 36):
 *   glass      = middle half  [36+δ, 108+δ)   center = 72+δ, δ = 5
 *   glass rates: Q1' → a=5 (upper), Q2' → a=7 (lower)
 *   outer rates (hyperbolic side, INVERTED):
 *                Q0' → a=103 (= inv 7), Q3' → a=29 (= inv 5)
 *
 * Antipodal inversion (exact, with offset): for ALL w ∈ [0,144)
 *   a_w × a_{(w+72)%144} ≡ 1 (mod 144)
 *   (peak of glass = trough of opposite side — sim_kis_hyperbolic.py)
 *
 * Addressing: logical local l sits at physical p = (a_w·l + b_w) % 144,
 *   b_w = (13·w) % 144 breaks ties → 144 distinct bijective views.
 * Data appended at scale w0 once; the passive scale-change log (hyperbolic
 * side) records every hop; replay telescopes → lossless at ANY read point.
 *
 * Proof:
 *   T0  window anchors: 20736÷4 = 5184 = 36×144; scale axis 144÷4 = 36
 *   T1  glass = middle [36+δ,108+δ), center 72+δ (offset visible)
 *   T2  inverted rates: a_w × a_{w+72} ≡ 1 mod 144, ALL 144 w
 *   T3  144 distinct bijective views (a,b pairs unique)
 *   T4  append at w0 → read at w0 (matching scale) → lossless
 *   T5  walk all 144 positions → lossless at EVERY point (via log)
 *   T6  far point without replay → mismatch (lossy-looking view)
 *   T7  passive log tiny; full-hop log telescopes to one entry
 *   T8  anchor: 18 tesseracts × 8 cubes × 144 = 20736 (future)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_magnify tests/test_tess_magnify.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define T1M_LOCAL       144u          /* scale positions W ∈ [0,144)   */
#define T1M_CUBES       8u
#define T1M_TOTAL       (T1M_CUBES * T1M_LOCAL)      /* 1152          */
#define T1M_INDEX_CUBE  0u
#define T1M_DATA_CUBES  (T1M_CUBES - 1u)             /* 7             */
#define T1M_DATA_SLOTS  (T1M_DATA_CUBES * T1M_LOCAL) /* 1008          */
#define T1M_BLOCK       18u
#define T1M_FULL        20736u
#define T1M_TESS_18     18u
#define T1M_QUAD        36u           /* 144 / 4                      */
#define T1M_OFFSET      5u            /* small +offset (นิดหน่อย)      */
#define T1M_HALF        72u           /* 144 / 2 — glass half-width    */

/* ── Magnify addressing: a_w from quadrant (with offset), b_w tie-break */
static uint8_t T1M_A[144], T1M_B[144], T1M_INV[144];

static int t1m_coeff_init(void) {
    for (uint32_t w = 0; w < T1M_LOCAL; w++) {
        uint32_t shifted = (w + T1M_LOCAL - T1M_OFFSET) % T1M_LOCAL;
        uint32_t q = shifted / T1M_QUAD;               /* 0..3           */
        uint8_t a;
        switch (q) {
            case 0: a = 103u; break;   /* Q0' outer — inverse of 7     */
            case 1: a = 5u;   break;   /* Q1' glass upper (magnify)    */
            case 2: a = 7u;   break;   /* Q2' glass lower (magnify)    */
            default: a = 29u; break;   /* Q3' outer — inverse of 5     */
        }
        T1M_A[w] = a;
        T1M_B[w] = (uint8_t)((w * 13u) % 144u);
        uint8_t inv = 0;
        for (uint32_t x = 1; x < 144 && inv == 0; x++)
            if (((uint32_t)a * x) % 144u == 1u) inv = (uint8_t)x;
        if (inv == 0) return -1;
        T1M_INV[w] = inv;
    }
    return 0;
}

/* ── Passive scale-change log (hyperbolic side) ─────────────────── */
typedef struct {
    uint8_t from;
    uint8_t to;
} ScaleEvent;

#define T1M_LOG_CAP 256u
typedef struct {
    ScaleEvent e[T1M_LOG_CAP];
    uint32_t   n;
} ScaleLog;

static void t1m_log_append(ScaleLog *log, uint32_t from, uint32_t to) {
    if (log->n >= T1M_LOG_CAP) return;
    log->e[log->n].from = (uint8_t)from;
    log->e[log->n].to   = (uint8_t)to;
    log->n++;
}

/* ── Store ──────────────────────────────────────────────────────── */
static uint32_t t1m_slot(uint32_t cube, uint32_t w) {
    return cube * T1M_LOCAL + w;
}

static uint8_t t1m_value(uint32_t cube, uint32_t local) {
    return (uint8_t)((cube * 37u + local * 7u + 11u) % 251u);
}

static uint32_t t1m_phys(uint32_t l, uint32_t w) {
    return ((uint32_t)T1M_A[w] * l + T1M_B[w]) % T1M_LOCAL;
}

static void t1m_encode(uint8_t *store, uint32_t w0) {
    memset(store, 0, T1M_TOTAL);
    uint8_t *frame = store + T1M_INDEX_CUBE * T1M_LOCAL;

    for (uint32_t c = 1; c < T1M_CUBES; c++) {
        uint32_t sum = 0;
        for (uint32_t l = 0; l < T1M_LOCAL; l++) {
            uint8_t v = t1m_value(c, l);
            store[t1m_slot(c, t1m_phys(l, w0))] = v;
            sum += v;
        }
        uint8_t *b = frame + c * T1M_BLOCK;
        uint32_t base = t1m_slot(c, 0);
        b[0] = (uint8_t)(base & 0xFFu);
        b[1] = (uint8_t)((base >> 8) & 0xFFu);
        b[2] = (uint8_t)(T1M_LOCAL & 0xFFu);
        b[3] = (uint8_t)((T1M_LOCAL >> 8) & 0xFFu);
        b[4] = (uint8_t)(sum % 251u);
        memset(b + 5, 0, T1M_BLOCK - 5u);
    }
}

/* replay log (forward 0..n−1): map append-view index → current-view index */
static uint32_t t1m_replay(uint32_t l, const ScaleLog *log) {
    uint32_t le = l % T1M_LOCAL;
    for (uint32_t i = 0; i < log->n; i++) {
        uint32_t f = log->e[i].from, t = log->e[i].to;
        int64_t num = (int64_t)T1M_A[f] * (int64_t)le + (int64_t)T1M_B[f] - (int64_t)T1M_B[t];
        num %= (int64_t)T1M_LOCAL;
        if (num < 0) num += T1M_LOCAL;
        le = (uint32_t)((num * T1M_INV[t]) % T1M_LOCAL);
    }
    return le;
}

static uint8_t t1m_read(const uint8_t *store, uint32_t cube, uint32_t l,
                        const ScaleLog *log, uint32_t cur_w) {
    uint32_t le = (log->n == 0) ? l : t1m_replay(l, log);
    return store[t1m_slot(cube, t1m_phys(le, cur_w))];
}

static int t1m_verify(const uint8_t *store, const ScaleLog *log, uint32_t cur_w) {
    const uint8_t *frame = store + T1M_INDEX_CUBE * T1M_LOCAL;
    for (uint32_t c = 1; c < T1M_CUBES; c++) {
        const uint8_t *b = frame + c * T1M_BLOCK;
        uint32_t sum = 0;
        for (uint32_t l = 0; l < T1M_LOCAL; l++) {
            if (t1m_read(store, c, l, log, cur_w) != t1m_value(c, l)) return 0;
            sum += t1m_value(c, l);
        }
        if ((sum % 251u) != b[4]) return 0;
    }
    return 1;
}

static uint32_t t1m_mismatch_count(const uint8_t *store, uint32_t cur_w) {
    ScaleLog empty;
    memset(&empty, 0, sizeof(empty));
    uint32_t bad = 0;
    for (uint32_t c = 1; c < T1M_CUBES; c++)
        for (uint32_t l = 0; l < T1M_LOCAL; l++)
            if (t1m_read(store, c, l, &empty, cur_w) != t1m_value(c, l)) bad++;
    return bad;
}

int main(void) {
    uint32_t pass = 0, fail = 0;
#define CHECK(d, c) do { if (c) { pass++; printf("  T: PASS — %s\n", d); } \
    else { fail++; printf("  T: FAIL — %s\n", d); } } while (0)

    if (t1m_coeff_init() != 0) { printf("  T: FAIL — coeff init\n"); return 1; }

    printf("Magnify Glass (20736÷4) × 1 Tesseract — inverted rate, +offset\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    /* T0: window anchors — 20736÷4 = 5184; scale axis 144÷4 = 36 */
    {
        CHECK("T0: 20736 ÷ 4 = 5184 = 36 scales × 144 vertices (quadrant)",
              T1M_FULL / 4u == 5184u && 5184u == T1M_QUAD * 144u);
        CHECK("T0b: 1-tess scale axis 144 ÷ 4 = 36 per quadrant",
              T1M_LOCAL / 4u == T1M_QUAD);
    }

    /* T1: glass = middle half [36+δ, 108+δ), center 72+δ */
    {
        int glass_ok = 1, outer_ok = 1;
        for (uint32_t w = 0; w < T1M_LOCAL; w++) {
            uint32_t shifted = (w + T1M_LOCAL - T1M_OFFSET) % T1M_LOCAL;
            int in_glass = (shifted >= T1M_QUAD && shifted < 3u * T1M_QUAD);
            if (in_glass && T1M_A[w] != 5u && T1M_A[w] != 7u) glass_ok = 0;
            if (!in_glass && T1M_A[w] != 29u && T1M_A[w] != 103u) outer_ok = 0;
        }
        CHECK("T1: glass = middle half (rate 5/7), outer = compressed (29/103)",
              glass_ok && outer_ok);
        printf("     glass center = 72+δ = %u (offset δ=%u, นิดหน่อย)\n",
               72u + T1M_OFFSET, T1M_OFFSET);
        CHECK("T1b: glass center = 72+δ (offset visible)",
              (72u + T1M_OFFSET) != 72u);
    }

    /* T2: inverted rates — a_w × a_{w+72} ≡ 1 mod 144, ALL w */
    {
        int ok = 1;
        for (uint32_t w = 0; w < T1M_LOCAL && ok; w++) {
            uint32_t antipode = (w + T1M_HALF) % T1M_LOCAL;
            if (((uint32_t)T1M_A[w] * T1M_A[antipode]) % 144u != 1u) ok = 0;
        }
        CHECK("T2: a_w × a_{w+72} ≡ 1 mod 144 for ALL 144 w (antipodal inversion)",
              ok);
        printf("     sample: a_%u=5 × a_%u=29 → 145 ≡ 1 ✓\n",
               (uint32_t)(36u + T1M_OFFSET), (uint32_t)(108u + T1M_OFFSET));
    }

    /* T3: 144 distinct bijective views */
    {
        int ok = 1;
        for (uint32_t w = 0; w < T1M_LOCAL && ok; w++) {
            if (T1M_A[w] == 0 || (T1M_A[w] % 2u) == 0 || (T1M_A[w] % 3u) == 0) { ok = 0; break; }
            uint8_t seen[T1M_LOCAL] = {0};
            for (uint32_t l = 0; l < T1M_LOCAL; l++) {
                uint32_t p = t1m_phys(l, w);
                if (seen[p]) { ok = 0; break; }
                seen[p] = 1;
            }
        }
        for (uint32_t w = 0; w < T1M_LOCAL && ok; w++)
            for (uint32_t w2 = w + 1; w2 < T1M_LOCAL && ok; w2++)
                if (T1M_A[w] == T1M_A[w2] && T1M_B[w] == T1M_B[w2]) ok = 0;
        CHECK("T3: 144 distinct bijective views (all gcd(a,144)=1)", ok);
    }

    uint8_t *store = (uint8_t *)calloc(T1M_TOTAL, 1);
    if (!store) { printf("  T: FAIL — alloc\n"); return 1; }

    uint32_t w0 = 0;
    t1m_encode(store, w0);

    /* T4: append at w0 → read at w0 (matching scale) → lossless */
    {
        ScaleLog empty;
        memset(&empty, 0, sizeof(empty));
        CHECK("T4: append at w=0 → read at w=0 (matching scale) lossless",
              t1m_verify(store, &empty, w0) == 1);
    }

    /* T5: walk ALL 144 positions → lossless at EVERY point via log */
    {
        ScaleLog log;
        memset(&log, 0, sizeof(log));
        uint32_t ok_pos = 0;
        uint32_t prev_w = w0;
        for (uint32_t w = 0; w < T1M_LOCAL; w++) {
            if (w != prev_w) t1m_log_append(&log, prev_w, w);
            if (t1m_verify(store, &log, w)) ok_pos++;
            prev_w = w;
        }
        CHECK("T5: 144/144 scale points → retrieve 8 cubes lossless (via log)",
              ok_pos == T1M_LOCAL);
        printf("     walk: %u points × %u slots = %u checks, log %u events\n",
               T1M_LOCAL, T1M_DATA_SLOTS, T1M_LOCAL * T1M_DATA_SLOTS, log.n);
    }

    /* T6: far point (opposite side) without replay → mismatch */
    {
        uint32_t far_w = (w0 + T1M_HALF + 17u) % T1M_LOCAL;   /* outside glass */
        uint32_t bad = t1m_mismatch_count(store, far_w);
        CHECK("T6: read on opposite side without replay → mismatch (lossy view)", bad > 0);
        printf("     mismatches at w=%u without replay: %u/%u\n", far_w, bad, T1M_DATA_SLOTS);
    }

    /* T7: log tiny + telescoping */
    {
        ScaleLog full;
        memset(&full, 0, sizeof(full));
        for (uint32_t w = 1; w < T1M_LOCAL; w++) t1m_log_append(&full, w - 1, w);
        uint32_t last_w = T1M_LOCAL - 1;

        ScaleLog one;
        memset(&one, 0, sizeof(one));
        t1m_log_append(&one, w0, last_w);

        uint32_t log_bytes = (uint32_t)(full.n * sizeof(ScaleEvent));
        printf("\n     full-hop log = %u events × %u B = %u bytes vs data %u slots\n",
               full.n, (unsigned)sizeof(ScaleEvent), log_bytes, T1M_DATA_SLOTS);
        CHECK("T7: passive log tiny vs stored data (delta ∝ events)",
              log_bytes < T1M_DATA_SLOTS);
        CHECK("T7b: full-hop log telescopes to single {w0→w} — identical result",
              t1m_verify(store, &full, last_w) == 1 &&
              t1m_verify(store, &one,  last_w) == 1);
    }

    /* T8: anchor — 18 tesseracts × 8 cubes × 144 = 20736 (future) */
    CHECK("T8: 18 tesseracts × 8 cubes × 144 = 20736 (future upgrade)",
          T1M_TESS_18 * T1M_CUBES * T1M_LOCAL == T1M_FULL);

    free(store);

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
