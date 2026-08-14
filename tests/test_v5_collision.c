/* ═══════════════════════════════════════════════════════════════════════════
 * test_v5_collision.c — Why the extracted v5 codec breaks on real data
 *                        (and the KIS timeline codec does not)
 *
 * User's observation: "codec กับข้อมูลสังเคราะห์ work แต่พอเจอข้อมูลจริงพัง
 *                      แต่ถ้าอยู่ใน kis timeline ไม่พัง"
 *
 * Root cause found (verified by probe):
 *   v5_decode reconstructs by walking slots in order and assigning
 *   out[index] = sorted[si] where si = slot-rank. This is only correct
 *   when slot order == sorted order. Any linear-probe collision (value
 *   duplication → probing → reorder) breaks the guarantee.
 *   → v5 roundtrips ONLY for small/nearly-sorted inputs (n=144 probe:
 *     LOSSLESS ratio 0.354; n=1000+ with duplicates: MISMATCH).
 *
 * Tests:
 *   T1  v5 on small synthetic (fits, spread)      → works  (the "synthetic works")
 *   T2  v5 on large synthetic (duplicates → probe) → breaks (the "real breaks" cause)
 *   T3  v5 on real GGUF slice + capacity vs 20736  → breaks
 *   T4  timeline scale coordinate w = (i·37)%144   → collision spread ~144×
 *   T5  timeline windowing (≤20736 per window)     → fixes capacity, not v5's bug
 *   T6  CONTROL — v6 (the timeline codec) on the same real slice → LOSSLESS
 *       (this is the "inside the KIS timeline it doesn't break" half)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test_v5_collision tests/test_v5_collision.c
 * RUN:   ./build/test_v5_collision [model.gguf] [tensor_name]
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "kis_codec_v5.h"
#include "kis_codec_v6.h"
#include "gguf_reader.h"

#define WINDOW_SLOTS    V5_SLOTS          /* 20736 — the KIS window */
#define TIMELINE_POS    144u              /* w positions per cell   */
#define LOAD_CAP        1000000u          /* max weights to load    */

static int errors = 0;
#define CHECK(desc, cond) do {                                          \
    if (cond) { printf("  ✅ %s\n", desc); }                            \
    else      { printf("  ❌ %s\n", desc); errors++; }                  \
} while (0)

/* ── Q8_0: 34B block (2B fp16 scale + 32 × int8) → int8 codes ──
 * Direct copy from the bulk-mapped base (gguf_read_tensor caps at tensor
 * size, so for partial reads we go straight to the mapping). */
static uint32_t read_q8_weights(const char *path, const char *tensor,
                                int8_t *out, uint32_t cap) {
    GgufReader r;
    if (gguf_open(path, &r) != 0) {
        printf("  (cannot open %s)\n", path);
        return 0;
    }
    int found = -1;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        if (r.names[i] && strcmp(r.names[i], tensor) == 0) { found = (int)i; break; }
    }
    if (found < 0) {
        for (uint32_t i = 0; i < r.n_tensors; i++) {
            if (r.dtypes[i] == 8) { found = (int)i; break; }
        }
        if (found >= 0) printf("  (tensor '%s' not found; using first Q8_0: %s)\n",
                               tensor, r.names[found]);
    }
    if (found < 0) {
        printf("  (no Q8_0 tensor in %s)\n", path);
        gguf_close(&r);
        return 0;
    }
    uint64_t total_weights = (uint64_t)r.sizes[found] / 34u * 32u;

    uint32_t want_blocks = (uint32_t)((cap / 32u) + 1u);
    uint32_t need = want_blocks * 34u;
    uint32_t have = (r.sizes[found] < need) ? r.sizes[found] : need;
    const uint8_t *src = r.base + r.data_offset + r.offsets[found];
    uint8_t *buf = (uint8_t *)malloc(have ? have : 1);
    memcpy(buf, src, have);
    gguf_close(&r);

    uint32_t n = 0;
    uint32_t n_blocks = have / 34u;
    for (uint32_t b = 0; b < n_blocks && n + 32 <= cap; b++) {
        for (int k = 0; k < 32; k++) out[n++] = (int8_t)buf[b * 34u + 2u + (uint32_t)k];
    }
    free(buf);
    printf("  tensor total weights = %llu (read %u)\n",
           (unsigned long long)total_weights, n);
    return n;
}

/* ── v5 exact probing simulation: collision metrics ── */
static void measure_probes(const int8_t *w, uint32_t n,
                           uint32_t *max_chain, uint64_t *total_probes,
                           uint32_t *distinct_codes) {
    uint8_t *used = (uint8_t *)calloc(V5_SLOTS, 1);
    uint32_t mx = 0;
    uint64_t probes = 0;
    uint8_t seen[256] = {0};
    uint32_t dist = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)w[i];
        if (!seen[c]) { seen[c] = 1; dist++; }
        uint32_t th, ph;
        v5_angular(c, &th, &ph);
        uint32_t s = (uint32_t)th * V5_GRID + ph;
        uint32_t chain = 0;
        while (used[s]) { s = (s + 1u) % V5_SLOTS; chain++; }
        used[s] = 1;
        if (chain > mx) mx = chain;
        probes += chain;
    }
    *max_chain = mx; *total_probes = probes; *distinct_codes = dist;
    free(used);
}

/* ── timeline scale coordinate w = (i·37)%144: per-cell×w occupancy ── */
static uint32_t timeline_max_bucket(const int8_t *w, uint32_t n) {
    uint32_t B = V5_SLOTS * TIMELINE_POS;              /* 20736 × 144 */
    uint16_t *occ = (uint16_t *)calloc(B, sizeof(uint16_t));
    uint32_t mx = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)w[i];
        uint32_t th, ph;
        v5_angular(c, &th, &ph);
        uint32_t cell = (uint32_t)th * V5_GRID + ph;
        uint32_t wpos = (i * 37u) % TIMELINE_POS;
        uint32_t b = cell * TIMELINE_POS + wpos;
        uint16_t v = ++occ[b];
        if (v > mx) mx = v;
    }
    free(occ);
    return mx;
}

/* ── codec roundtrip (v5 or v6); expect_ok = what the story predicts ── */
static void roundtrip(const char *label, const char *which, int expect_ok,
                      const int8_t *w, uint32_t n) {
    uint32_t cap = n + 4u * WINDOW_SLOTS + 2048u;
    uint8_t *buf = (uint8_t *)malloc(cap);
    int8_t *dec = (int8_t *)malloc(n);
    uint32_t off;
    int rc;
    if (strcmp(which, "v6") == 0) {
        off = v6_encode(w, n, buf, cap);
        rc = v6_decode(buf, off, dec, n);
    } else {
        off = v5_encode(w, n, buf, cap);
        rc = v5_decode(buf, off, dec, n);
    }
    int ok = (rc == 0 && memcmp(w, dec, n) == 0);
    printf("  [%s] %-34s n=%-7u enc=%uB ratio=%.3f %s\n",
           which, label, n, off, (double)off / (double)n,
           ok ? "LOSSLESS" : (rc ? "FAIL" : "MISMATCH"));
    CHECK(label, ok == expect_ok);
    free(buf); free(dec);
}

/* ── timeline windowed chain: whole stream, window by window ── */
static void windowed_chain(const char *label, const int8_t *w, uint32_t n,
                           uint32_t win, int expect_ok) {
    uint64_t total_enc = 0;
    uint32_t n_win = 0, n_bad = 0;
    for (uint32_t base = 0; base < n; base += win) {
        uint32_t cnt = (n - base < win) ? (n - base) : win;
        uint32_t cap = cnt + 4u * WINDOW_SLOTS + 2048u;
        uint8_t *buf = (uint8_t *)malloc(cap);
        int8_t *dec = (int8_t *)malloc(cnt);
        uint32_t off = v5_encode(w + base, cnt, buf, cap);
        int rc = v5_decode(buf, off, dec, cnt);
        if (rc != 0 || memcmp(w + base, dec, cnt) != 0) n_bad++;
        total_enc += off;
        n_win++;
        free(buf); free(dec);
    }
    /* split printf calls — one conversion class per call (mingw CRT quirk) */
    printf("  %-38s n=%-7u windows=%-4u ", label, n, n_win);
    if (n_bad) printf("bad=%-4u ", n_bad);
    printf("enc=%lluB ", (unsigned long long)total_enc);
    printf("ratio=%.3f ", (double)total_enc / (double)n);
    if (n_bad) printf("[FAIL] %u windows broken\n", n_bad);
    else printf("[OK] all windows lossless\n");
    CHECK(label, (n_bad == 0) == expect_ok);
}

int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *tensor = (argc > 2) ? argv[2] : "token_embd.weight";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("v5 collision experiment — synthetic vs real, timeline fix\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    /* T1: the case where v5 genuinely works — probe-verified:
     * few distinct codes + sorted + small → slot order == sorted order. */
    printf("[T1] v5 on small sorted synthetic (n=144, few codes) — 'synthetic works'\n");
    {
        int8_t w[144];
        for (int i = 0; i < 78; i++) w[i] = (int8_t)-128;   /* few distinct */
        for (int i = 78; i < 144; i++) w[i] = (int8_t)-127; /* + sorted */
        roundtrip("small sorted synthetic", "v5", 1, w, 144);
    }

    /* T2: v5 on large synthetic — duplicates → probing → decode guarantee broken */
    printf("\n[T2] v5 on large synthetic (n=10000, duplicates) — 'real-like' break\n");
    {
        int8_t *w = (int8_t *)malloc(10000);
        srand(42);
        for (uint32_t i = 0; i < 10000; i++) w[i] = (int8_t)(rand() % 256);
        uint32_t mx, dist; uint64_t probes;
        measure_probes(w, 10000, &mx, &probes, &dist);
        printf("  distinct codes=%u  max_probe_chain=%u  total_probes=%llu\n",
               dist, mx, (unsigned long long)probes);
        roundtrip("random large synthetic", "v5", 0, w, 10000);

        /* strictly sorted — still breaks once probes reorder slots */
        static int8_t sorted[10000];
        int h[256] = {0};
        for (uint32_t i = 0; i < 10000; i++) h[(uint8_t)w[i]]++;
        int p = 0;
        for (int v = -128; v < 128; v++)
            for (int k = 0; k < h[(uint8_t)v]; k++) sorted[p++] = (int8_t)v;
        roundtrip("sorted large synthetic", "v5", 0, sorted, 10000);
        free(w);
    }

    /* T4: timeline coordinate spreads collisions */
    printf("\n[T4] timeline scale coordinate w = (i·37)%%144 — collision spread\n");
    {
        int8_t *w = (int8_t *)malloc(10000);
        srand(42);
        for (uint32_t i = 0; i < 10000; i++) w[i] = (int8_t)(rand() % 256);
        uint32_t mx, dist; uint64_t probes;
        measure_probes(w, 10000, &mx, &probes, &dist);
        uint32_t tb = timeline_max_bucket(w, 10000);
        printf("  code-only:     max_probe_chain=%u  (values collide in one cell)\n", mx);
        printf("  with w coord:  max bucket=%u  → spread factor ≈ %u×\n",
               tb, mx / (tb ? tb : 1));
        CHECK("timeline coordinate spreads collisions", tb <= 4);
        free(w);
    }

    /* T3/T5/T6: real data */
    printf("\n[T3] REAL — %s @ %s\n", tensor, gguf);
    int8_t *real = (int8_t *)malloc(LOAD_CAP);
    uint32_t n_real = read_q8_weights(gguf, tensor, real, LOAD_CAP);
    if (n_real == 0) {
        printf("  ⚠  real GGUF unavailable — skipping T3/T5/T6\n");
        free(real);
        printf("\n═══════════════════════════════════════════════════════════════\n");
        printf(errors ? "❌ %d FAILED\n" : "✅ ALL PASSED (synthetic only)\n", errors);
        return errors;
    }

    /* capacity: the other half of "real breaks" — n >> 20736 */
    printf("\n  WINDOW CAPACITY: %u slots — real tensor is %s the window\n",
           WINDOW_SLOTS, n_real > WINDOW_SLOTS ? "LARGER THAN" : "within");
    if (n_real > WINDOW_SLOTS) {
        printf("  → v5_encode cannot run on the full tensor (grid %u < n %u):\n",
               WINDOW_SLOTS, n_real);
        printf("    hard capacity break — the timeline's answer is WINDOWING (T5).\n");
        CHECK("capacity violation detected (n > 20736)", 1);
    }

    printf("\n  — one-window slice (first %u weights) —\n", WINDOW_SLOTS);
    {
        uint32_t sl = n_real < WINDOW_SLOTS ? n_real : WINDOW_SLOTS;
        uint32_t mx, dist; uint64_t probes;
        measure_probes(real, sl, &mx, &probes, &dist);
        printf("  distinct codes=%u  max_probe_chain=%u  total_probes=%llu\n",
               dist, mx, (unsigned long long)probes);
        roundtrip("v5 on real one-window slice", "v5", 0, real, sl);
        roundtrip("v6 on real one-window slice", "v6", 1, real, sl);
    }

    /* T5: windowing fixes capacity but not v5's order bug */
    printf("\n[T5] timeline windowing on the whole real stream\n");
    windowed_chain("v5 windowed (20736/window)", real, n_real, WINDOW_SLOTS, 0);

    /* T6: control — the timeline's own codec on the same stream */
    printf("\n[T6] CONTROL — the KIS timeline codec (v6) on the same real stream\n");
    {
        uint64_t total_enc = 0;
        uint32_t n_win = 0;
        int bad = 0;
        for (uint32_t base = 0; base < n_real && !bad; base += WINDOW_SLOTS) {
            uint32_t cnt = (n_real - base < WINDOW_SLOTS) ? (n_real - base) : WINDOW_SLOTS;
            uint32_t cap = cnt + 4u * WINDOW_SLOTS + 2048u;
            uint8_t *buf = (uint8_t *)malloc(cap);
            int8_t *dec = (int8_t *)malloc(cnt);
            uint32_t off = v6_encode(real + base, cnt, buf, cap);
            int rc = v6_decode(buf, off, dec, cnt);
            if (rc != 0 || memcmp(real + base, dec, cnt) != 0) bad = 1;
            total_enc += off;
            n_win++;
            free(buf); free(dec);
        }
        /* split printf calls — one conversion class per call (mingw CRT quirk) */
        printf("  %-38s n=%-7u windows=%-4u ", "v6 windowed (20736/window)", n_real, n_win);
        printf("enc=%lluB ", (unsigned long long)total_enc);
        printf("ratio=%.3f ", (double)total_enc / (double)n_real);
        if (bad) printf("[FAIL] broken\n");
        else printf("[OK] all windows lossless\n");
        CHECK("v6 timeline codec lossless on real stream", !bad);
    }

    free(real);

    printf("\n═══════════════════════════════════════════════════════════════\n");
    if (errors == 0) printf("✅ ALL TESTS PASSED\n");
    else printf("❌ %d TEST(S) FAILED\n", errors);
    return errors;
}
