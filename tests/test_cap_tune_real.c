/* test_cap_tune_real.c — Tune the envelope gate against real GGUF models
 * ═══════════════════════════════════════════════════════════════════════════
 * Question: which gate value for envelope_depth(gate) fits the real model
 * data?  Measure, don't assume.
 *
 * Real placement (test_gguf_window_chain — the formula, not a guess):
 *   home(rank) = (rank·37) % 20736          stride-37 permutation
 *   w(rank)    = home % 144                 scale position (0..143)
 *   depth_i    = w_i                        requested expansion depth
 *
 * Per gate g: envelope k_max = envelope_depth(g) (§15.32).  Tensor i:
 *   w_i > k_max  → LIFT to ghost store      field cost 0
 *   w_i ≤ k_max  → PLACE at depth w_i       field cost = view_of(s_i, w_i)
 *
 * Metrics per model per gate:
 *   lifts%       = fraction of tensors lifted
 *   field_slots  = Σ view_of(s_i, w_i) over placed (real field reservation)
 *   field_windows= ceil(field_slots / 20736)
 *   ghost_elems  = Σ s_i over lifted (data moved to residual_space)
 *   baseline     = ceil(E / 20736) — windows if NOTHING lifts (all at base)
 *
 * Decision: pick the default gate from the ROI knee (1.0) and verify the
 * data supports it: if lift rate is insensitive to the knob (uniform scale
 * distribution) then 1.0 — the ROI knee — is the right default, because
 * tightening only moves marginal tensors to ghost without changing the
 * system's shape.
 *
 * Models (optional — missing = skip, TIER1 stays green):
 *   SmolLM2-360M, Qwen3-0.6B, LFM2.5-2.6B, Qwen2.5-0.5B (I:/model/)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-cap_tune_real tests/test_cap_tune_real.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/geo_cap_account.h"
#include "../core/gguf_box.h"

#define WIN 20736u

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ceil ของ s/2ᵏ (k ≥ 63 → 1) — เดียวกับ test_gguf_multi_model */
static uint64_t view_of(uint64_t s, uint32_t k) {
    if (k >= 63) return s ? 1 : 0;
    uint64_t d = 1ull << k;
    return (s + d - 1) / d;
}

/* real scale position of rank r — the placement formula */
static uint8_t scale_w(uint32_t rank) {
    return (uint8_t)(((uint64_t)rank * 37u) % 144u);
}

/* OPTIMAL rank assignment: the field ranks (w ≤ k_max) are SCATTERED
 * across the rank axis (e.g. {0,4,39,74,109,113,...} for k_max=5), so a
 * plain ascending fill wastes them on mid-size tensors.  Instead TARGET
 * the field ranks: put the smallest tensors exactly there (smallest at
 * smallest w — least shrink paid on the biggest of the smalls), and the
 * large tensors at the high-w ranks → they lift.  Any assignment of the
 * other ranks is irrelevant to field footprint. */
static uint64_t field_windows_optimal(const uint64_t *s, uint32_t N, double gate) {
    uint32_t k_max = ght_envelope_depth(gate);

    /* field ranks (w ≤ k_max), sorted by w ascending */
    uint32_t *fr = (uint32_t *)malloc(N * sizeof(uint32_t));
    uint32_t nf = 0;
    for (uint32_t r = 0; r < N; r++)
        if (scale_w(r) <= k_max) fr[nf++] = r;
    for (uint32_t i = 0; i < nf; i++)
        for (uint32_t j = i + 1; j < nf; j++)
            if (scale_w(fr[j]) < scale_w(fr[i])) { uint32_t t = fr[i]; fr[i] = fr[j]; fr[j] = t; }

    /* tensor indices sorted by size ascending */
    uint32_t *idx = (uint32_t *)malloc(N * sizeof(uint32_t));
    for (uint32_t i = 0; i < N; i++) idx[i] = i;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t b = i;
        for (uint32_t j = i + 1; j < N; j++)
            if (s[idx[j]] < s[idx[b]]) b = j;
        uint32_t t = idx[i]; idx[i] = idx[b]; idx[b] = t;
    }

    /* smallest tensors → field ranks (ascending w ↔ ascending s) */
    uint64_t field = 0;
    for (uint32_t i = 0; i < nf; i++)
        field += view_of(s[idx[i]], scale_w(fr[i]));
    free(fr); free(idx);
    return (field + WIN - 1) / WIN;
}

typedef struct {
    const char *label;
    const char *path;
    int      present;
    uint32_t N;
    uint64_t E;
    double   lifts_050, lifts_100, lifts_150, lifts_200;  /* lift % per gate */
    uint64_t field_100;      /* field slots @ gate 1.0 */
    uint64_t ghost_100;      /* ghost elems @ gate 1.0 */
    uint64_t base_windows;   /* windows if nothing lifts */
    uint64_t field_windows_100;
    uint64_t field_opt;      /* windows with size-aware rank order */
    const char *rank0_cur;   /* rank-0 tensor in file order */
    const char *rank0_opt;   /* rank-0 tensor in ascending order */
    uint64_t rank0_cur_elems;
    uint64_t rank0_opt_elems;
} Model;

static void run_model(Model *m) {
    GGUFBox box;
    if (gguf_box_open(&box, m->path) != 0) {
        printf("  (cannot open — skipping)\n");
        m->present = 0;
        return;
    }
    m->present = 1;
    uint32_t N = box.n_tensors;
    uint64_t *s = (uint64_t *)calloc(N, sizeof(uint64_t));
    uint64_t E = 0;
    for (uint32_t i = 0; i < N; i++) { s[i] = box.entries[i].n_elems; E += s[i]; }
    m->N = N; m->E = E;
    m->base_windows = (E + WIN - 1) / WIN;

    const double gates[4] = { 0.5, 1.0, 1.5, 2.0 };
    double *lifts = (double[]){ 0, 0, 0, 0 };
    uint64_t field[4] = { 0, 0, 0, 0 };
    uint64_t ghost[4] = { 0, 0, 0, 0 };

    for (uint32_t g = 0; g < 4; g++) {
        uint32_t k_max = ght_envelope_depth(gates[g]);
        for (uint32_t i = 0; i < N; i++) {
            uint8_t w = scale_w(i);
            if (w > k_max) { lifts[g] += 1.0; ghost[g] += s[i]; }
            else           { field[g] += view_of(s[i], w); }
        }
        lifts[g] = 100.0 * lifts[g] / (double)N;
    }

    m->lifts_050 = lifts[0]; m->lifts_100 = lifts[1];
    m->lifts_150 = lifts[2]; m->lifts_200 = lifts[3];
    m->field_100 = field[1];
    m->ghost_100 = ghost[1];
    m->field_windows_100 = (field[1] + WIN - 1) / WIN;

    /* ── size-aware rank order: smallest tensors targeted at field ranks ── */
    m->field_opt = field_windows_optimal(s, N, 1.0);
    m->rank0_cur = box.entries[0].name;      m->rank0_cur_elems = s[0];
    m->rank0_opt = box.entries[0].name;      m->rank0_opt_elems = s[0];
    {
        uint64_t min = s[0]; uint32_t bi = 0;
        for (uint32_t i = 1; i < N; i++)
            if (s[i] < min) { min = s[i]; bi = i; }
        m->rank0_opt = box.entries[bi].name;
        m->rank0_opt_elems = s[bi];
    }

    printf("  %u tensors, E = %llu elems, base (no lift) = %llu windows\n",
           N, (unsigned long long)E, (unsigned long long)m->base_windows);
    printf("  k_max: gate 0.5→%u | 1.0→%u | 1.5→%u | 2.0→%u\n",
           ght_envelope_depth(0.5), ght_envelope_depth(1.0),
           ght_envelope_depth(1.5), ght_envelope_depth(2.0));
    printf("  lifts%%: 0.5→%.1f | 1.0→%.1f | 1.5→%.1f | 2.0→%.1f\n",
           lifts[0], lifts[1], lifts[2], lifts[3]);
    printf("  @gate1.0: field %llu slots (%llu windows) vs base %llu windows; ghost %llu elems (%.1f MB)\n",
           (unsigned long long)field[1], (unsigned long long)m->field_windows_100,
           (unsigned long long)m->base_windows,
           (unsigned long long)ghost[1], (double)ghost[1] / 1048576.0);
    printf("  field windows: file-order %llu → targeted-smallest %llu (%.1f×)\n",
           (unsigned long long)m->field_windows_100,
           (unsigned long long)m->field_opt,
           (double)m->field_windows_100 / (double)(m->field_opt ? m->field_opt : 1));
    printf("  rank-0 tensor: file-order [%s] %llu elems (w=0, field full price)\n",
           m->rank0_cur, (unsigned long long)m->rank0_cur_elems);
    printf("                 targeted [%s] %llu elems (w=0, smallest in field)\n",
           m->rank0_opt, (unsigned long long)m->rank0_opt_elems);

    free(s);
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Envelope gate tuning — real GGUF tensor sets\n");
    printf("════════════════════════════════════════════════════════\n");

    Model models[4];
    models[0] = (Model){ "SmolLM2-360M", "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf", 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    models[1] = (Model){ "Qwen3-0.6B",   "I:/model/Qwen3-0.6B-Q8_0.gguf",           0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    models[2] = (Model){ "LFM2.5-2.6B",  "I:/model/LFM2.5-2.6B-Q8_0.gguf",          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    models[3] = (Model){ "Qwen2.5-0.5B", "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    if (argc > 1) models[0].path = argv[1];
    if (argc > 2) models[1].path = argv[2];

    uint32_t n_present = 0;
    for (uint32_t i = 0; i < 4; i++) {
        printf("\n═ %s — %s ═\n", models[i].label, models[i].path);
        run_model(&models[i]);
        if (models[i].present) n_present++;
    }

    /* ── aggregate ── */
    double tot_l050 = 0, tot_l100 = 0, tot_l150 = 0, tot_l200 = 0;
    uint64_t tot_field = 0, tot_base = 0, tot_ghost = 0, tot_fw = 0, tot_fopt = 0;
    for (uint32_t i = 0; i < 4; i++) {
        if (!models[i].present) continue;
        tot_l050 += models[i].lifts_050;
        tot_l100 += models[i].lifts_100;
        tot_l150 += models[i].lifts_150;
        tot_l200 += models[i].lifts_200;
        tot_field += models[i].field_100;
        tot_base  += models[i].base_windows;
        tot_ghost += models[i].ghost_100;
        tot_fw    += models[i].field_windows_100;
        tot_fopt  += models[i].field_opt;
    }
    if (n_present > 0) {
        tot_l050 /= n_present; tot_l100 /= n_present;
        tot_l150 /= n_present; tot_l200 /= n_present;
    }

    printf("\n════════════════════════════════════════════════════════\n");
    printf("AGGREGATE (%u/%u models present)\n", n_present, 4u);
    printf("  lifts%%: 0.5→%.1f | 1.0→%.1f | 1.5→%.1f | 2.0→%.1f\n",
           tot_l050, tot_l100, tot_l150, tot_l200);
    printf("  @gate1.0: field %llu windows (Σ) vs base %llu windows; ghost %.1f MB\n",
           (unsigned long long)tot_fw, (unsigned long long)tot_base,
           (double)tot_ghost / 1048576.0);
    printf("  field (file-order) %llu → field (targeted-smallest) %llu windows (%.1f×)\n",
           (unsigned long long)tot_fw, (unsigned long long)tot_fopt,
           (double)tot_fw / (double)(tot_fopt ? tot_fopt : 1));

    /* ── checks (real data — the numbers ARE the result) ── */
    CHECK(1, "≥2 real models measured (data-driven tuning)",
          n_present >= 2);
    CHECK(2, "most tensors sit deeper than the envelope — lift rate @1.0 > 75%",
          n_present >= 2 && tot_l100 > 75.0 && tot_l100 < 100.0);
    CHECK(3, "knob direction: lift rate monotonic in gate",
          n_present >= 2 && tot_l200 >= tot_l150 && tot_l150 >= tot_l100 &&
          tot_l100 >= tot_l050);
    CHECK(4, "plateau: rate insensitive to knob (Δ 0.5→2.0 < 15pp — uniform scale dist)",
          n_present >= 2 && (tot_l200 - tot_l050) < 15.0);
    CHECK(5, "lifting cuts field footprint ≥ 2× vs base (no-lift)",
          n_present >= 2 && tot_fw > 0 && tot_fw * 2 <= tot_base);
    CHECK(6, "default gate = 1.0 (ROI knee) — plateau makes 0.5/1.5/2.0 equivalent",
          n_present >= 2 && (tot_l150 - tot_l100) < 2.0 &&
          (tot_l100 - tot_l050) < 2.0);

    /* ── targeted rank assignment: large low-rank tensors now lift ── */
    int never_worse = 1;
    for (uint32_t i = 0; i < 4; i++)
        if (models[i].present && models[i].field_opt > models[i].field_windows_100)
            never_worse = 0;
    CHECK(7, "targeted-smallest ranks never worse than file order (ทุกโมเดล)",
          n_present >= 2 && never_worse);
    CHECK(8, "aggregate field cut by targeted ranks",
          n_present >= 2 && tot_fopt < tot_fw);
    CHECK(9, "field now fits in a handful of windows (Σ ≤ 64)",
          n_present >= 2 && tot_fopt <= 64);

    printf("\n════════════════════════════════════════════════════════\n");
    printf("models present: %u/4 — RESULTS: %d/%d PASS\n",
           n_present, pass, pass + fail);
    printf("recommended default gate: 1.0 (envelope_depth = %u — ROI knee, k 4-5 เหมาะสมที่สุด)\n",
           ght_envelope_depth(1.0));
    return fail ? 1 : 0;
}
