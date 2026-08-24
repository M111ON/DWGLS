/* test_gguf_multi_model.c — leverage gate × registry หลายโมเดลจริง
 *
 * รัน gate sweep (เดียวกันกับ test_gguf_real_gate) บนโมเดลหลายตัว
 * เพื่อตอบ: base/knee/registry-tax เปลี่ยนตามสถาปัตยกรรมยังไง?
 *
 *   model-level gate: storage(k) = ceil(E/2ᵏ/WIN) windows (chain ต่อเนื่อง)
 *     cost/step = drag (Σ chunks + replay events)
 *     ROI(k) = benefit(k)/cost — knee = จุดที่ขยายไม่คุ้ม (ROI < GATE)
 *   addressing tax: Σ_i ceil(ceil(s_i/2ᵏ)/WIN) ต่อ-tensor (registry span)
 *     real vs uniform — tensor เล็ก (norm/attn) ตกเพดาน 1 window
 *   small% = สัดส่วน tensor ที่ view หดแล้วไม่เต็ม window — ตัวขับ tax
 *
 * โมเดล (ทุกตัว optional — ไม่เจอ = skip, TIER1 ยังเขียว):
 *   SmolLM2-360M-Instruct.Q8_0  (Llama-style ขนาดเล็ก)
 *   Qwen3-0.6B-Q8_0             (Qwen3 arch)
 *   Qwen2.5-0.5B-Instruct-Q8_0  (baseline จาก §15.11 — ต้องได้ 7/128×/1.95×)
 *
 *   (LFM2.5-2.6B ถูกถอดจาก roster Aug 24 — ไม่ใช้แล้ว; X3 ที่เทียบ
 *    โมเดลใหญ่↔เล็ก จะ SKIP จนกว่าจะมีโมเดล ≥1.5G elements กลับเข้ามา)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_gguf_multi_model \
 *        tests/test_gguf_multi_model.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

#define WIN           20736u
#define FLOOR_MARGIN  2u
#define GATE          1.0

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── ceil ของ s/2ᵏ และ s/144 (chunks) — เหมือน real_gate ────── */
static uint64_t view_of(uint64_t s, uint32_t k) {
    if (k >= 63) return s ? 1 : 0;
    uint64_t d = 1ull << k;
    return (s + d - 1) / d;
}
static uint64_t chunks_of(uint64_t s) { return (s + 143u) / 144u; }
static uint64_t storage(uint64_t E, uint32_t k) {
    uint64_t v = view_of(E, k);
    return (v + WIN - 1) / WIN;
}
static uint64_t addr_spans(const uint64_t *s, uint32_t n, uint32_t k) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = view_of(s[i], k);
        sum += (v + WIN - 1) / WIN;
    }
    return sum;
}
static uint64_t drag_cost(const uint64_t *s, uint32_t n) {
    uint64_t c = 0;
    for (uint32_t i = 0; i < n; i++) c += chunks_of(s[i]);
    return c * 1u + (uint64_t)n * 8u;   /* EVENT_SLOT=1, REPLAY_EVENT=8 */
}
static double roi_step(uint64_t E, uint64_t cost, uint32_t k) {
    uint64_t b = storage(E, k) - storage(E, k + 1);
    return (double)(b * WIN) / (double)cost;
}

typedef struct {
    const char *label;      /* ชื่อในตาราง */
    const char *path;
    int      present;
    uint32_t N;
    uint64_t E;
    uint64_t cost;
    uint32_t base;          /* final base (knee) */
    uint64_t storage0, storage_b;
    double   comp;          /* base0/base */
    uint64_t spans_real, spans_uniform;
    double   tax_real, tax_uniform;
    double   small_pct;     /* % tensor ที่ view ≤ 1 window ที่ base */
    double   roi_first, roi_last;   /* ROI ที่ margin และที่ knee */
} Model;

static void run_model(Model *m) {
    GGUFBox box;
    printf("\n═ %s — %s ═\n", m->label, m->path);
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
    m->cost = drag_cost(s, N);
    m->storage0 = storage(E, 0);

    printf("  %u tensors, E = %llu elements, base0 = %llu windows\n",
           N, (unsigned long long)E, (unsigned long long)m->storage0);
    printf("  cost/step (drag) = %llu slots\n  k   storage(w)  saved(w)  ROI\n",
           (unsigned long long)m->cost);

    uint32_t final_base = 0;
    double roi_at[16];
    for (uint32_t k = FLOOR_MARGIN; k <= 12; k++) {
        roi_at[k] = roi_step(E, m->cost, k);
        printf("  %2u  %7llu  %7llu  %6.2f\n", k,
               (unsigned long long)storage(E, k),
               (unsigned long long)(storage(E, k) - storage(E, k + 1)),
               roi_at[k]);
        if (roi_at[k] >= GATE) final_base = k + 1;
    }
    m->base = final_base;
    m->storage_b = storage(E, final_base);
    m->comp = (double)m->storage0 / (double)m->storage_b;
    m->roi_first = roi_at[FLOOR_MARGIN];
    m->roi_last  = roi_at[final_base];

    /* addressing tax */
    uint64_t *u = (uint64_t *)calloc(N, sizeof(uint64_t));
    { uint64_t base = E / N, rem = E % N;
      for (uint32_t i = 0; i < N; i++) u[i] = base + (i < rem ? 1 : 0); }
    m->spans_real    = addr_spans(s, N, final_base);
    m->spans_uniform = addr_spans(u, N, final_base);
    m->tax_real    = (double)m->spans_real    / (double)m->storage_b;
    m->tax_uniform = (double)m->spans_uniform / (double)m->storage_b;

    /* สัดส่วน tensor เล็ก (view หดแล้วไม่เต็ม window) — ตัวขับ tax */
    uint64_t small = 0;
    for (uint32_t i = 0; i < N; i++) if (view_of(s[i], final_base) <= WIN) small++;
    m->small_pct = 100.0 * (double)small / (double)N;

    printf("  final base = %u, storage = %llu windows (%.0f×), spans real %llu (tax %.2f×) / uniform %llu (tax %.2f×), small-tensor %.0f%%\n",
           final_base, (unsigned long long)m->storage_b, m->comp,
           (unsigned long long)m->spans_real, m->tax_real,
           (unsigned long long)m->spans_uniform, m->tax_uniform, m->small_pct);

    /* ── per-model checks ── */
    char desc[160];
    snprintf(desc, sizeof(desc), "M1 [%s]: โมเดลมีขนาดจริง (N ≥ 100, E ≥ 30M)", m->label);
    CHECK(desc, N >= 100 && E >= 30000000ull);
    snprintf(desc, sizeof(desc), "M2 [%s]: compression ≥ 16× ที่ base จริง", m->label);
    CHECK(desc, m->storage0 >= 16ull * m->storage_b);
    snprintf(desc, sizeof(desc), "M3 [%s]: knee มีอยู่ — ROI(base) < 1, base ∈ (margin,12]", m->label);
    CHECK(desc, final_base > FLOOR_MARGIN && final_base <= 12 && roi_at[final_base] < GATE);
    int mono = 1;
    for (uint32_t k = FLOOR_MARGIN; k < 12; k++)
        if (roi_at[k] < roi_at[k + 1]) mono = 0;
    snprintf(desc, sizeof(desc), "M4 [%s]: ROI ลดลงตาม k (leverage แย่ลงทวีคูณ)", m->label);
    CHECK(desc, mono);
    snprintf(desc, sizeof(desc), "M5 [%s]: tax real ≥ tax uniform ≥ 1 (Σ ceil ≥ ceil Σ)", m->label);
    CHECK(desc, m->tax_real >= m->tax_uniform && m->tax_uniform >= 1.0);
    snprintf(desc, sizeof(desc), "M6 [%s]: มี tensor เล็กจริง (small%% > 0) — ที่มาของ tax > 1", m->label);
    CHECK(desc, m->small_pct > 0.0);

    free(s); free(u);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("GGUF multi-model gate — base/knee/registry-tax ตามสถาปัตยกรรม\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    Model models[3];
    models[0] = (Model){ "SmolLM2-360M", "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf", 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    models[1] = (Model){ "Qwen3-0.6B",   "I:/model/Qwen3-0.6B-Q8_0.gguf",           0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    models[2] = (Model){ "Qwen2.5-0.5B", "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    if (argc > 1) models[0].path = argv[1];
    if (argc > 2) models[1].path = argv[2];

    uint32_t n_present = 0;
    for (uint32_t i = 0; i < 3; i++) run_model(&models[i]);

    /* ── cross-model table ── */
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("═ สรุป — base/knee/tax ต่อสถาปัตยกรรม ═\n");
    printf("  %-14s %5s %12s %5s %7s %6s %8s %8s %7s %7s\n",
           "model", "N", "E(elem)", "base", "win@b", "comp×", "spansR", "spansU", "taxR", "taxU");
    for (uint32_t i = 0; i < 3; i++) {
        Model *m = &models[i];
        if (!m->present) { printf("  %-14s   (ไม่เจอไฟล์ — skip)\n", m->label); continue; }
        n_present++;
        printf("  %-14s %5u %12llu %5u %7llu %6.0f %8llu %8llu %7.2f %7.2f\n",
               m->label, m->N, (unsigned long long)m->E, m->base,
               (unsigned long long)m->storage_b, m->comp,
               (unsigned long long)m->spans_real, (unsigned long long)m->spans_uniform,
               m->tax_real, m->tax_uniform);
    }

    /* ── cross-model checks ── */
    /* X1: ทุกโมเดลที่เจอผ่าน gate ไปที่ knee ใกล้เคียงกัน (base 3..9) */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 3; i++)
            if (models[i].present && (models[i].base < 3 || models[i].base > 9)) ok = 0;
        CHECK("X1: ทุกโมเดล knee อยู่ในช่วง 3..9 (gate ทำงานข้ามสถาปัตยกรรม)", ok);
    }
    /* X2: compression ทุกตัว ≥ 64× (วางลึก = 128×, อนุญาต edge 1 ก้าว) */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 3; i++)
            if (models[i].present && models[i].comp < 64.0) ok = 0;
        CHECK("X2: ทุกโมเดล compression ≥ 64× (base ลึก = 128×±1 ก้าว)", ok);
    }
    /* X3: โมเดลใหญ่ → tax น้อยกว่า (tensor ใหญ่ → แตกน้อย)
       LFM2.5-2.6B ถอดจาก roster แล้ว — ถ้าไม่มีโมเดล ≥1.5G elements = SKIP
       (นับ skip แยกจาก pass — ไม่โกหกว่าผ่าน, ไม่แดงโดยไม่มีความผิดจริง) */
    {
        const Model *big = NULL, *sm = NULL;
        for (uint32_t i = 0; i < 3; i++) {
            if (!models[i].present) continue;
            if (models[i].E > 1500000000ull) big = &models[i];   /* ≥ 1.5G elements */
            if (models[i].E <  800000000ull) sm  = &models[i];   /* ≤ 800M elements */
        }
        if (big && sm)
            CHECK("X3: โมเดลใหญ่ tax ≤ โมเดลเล็ก (tensor ใหญ่แตกน้อยกว่า)",
                  big->tax_real <= sm->tax_real + 0.05);
        else
            printf("  T: SKIP — X3 (ต้องการโมเดลใหญ่ ≥1.5G elem; LFM2.5-2.6B ถอดจาก roster แล้ว)\n");
    }
    /* X4: determinism — รัน gate อีกครั้ง (ผ่าน path) ให้ base เดิม */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 3; i++) {
            if (!models[i].present) continue;
            GGUFBox box;
            if (gguf_box_open(&box, models[i].path) != 0) { ok = 0; continue; }
            uint32_t N = box.n_tensors;
            uint64_t *s = (uint64_t *)calloc(N, sizeof(uint64_t));
            uint64_t E = 0;
            for (uint32_t j = 0; j < N; j++) { s[j] = box.entries[j].n_elems; E += s[j]; }
            uint64_t cst = drag_cost(s, N);
            uint32_t b2 = 0;
            for (uint32_t k = FLOOR_MARGIN; k <= 12; k++)
                if (roi_step(E, cst, k) >= GATE) b2 = k + 1;
            if (b2 != models[i].base) ok = 0;
            free(s);
        }
        CHECK("X4: gate sweep รอบสอง → base เดิมทุกโมเดล (deterministic)", ok);
    }
    /* X5: Qwen2.5-0.5B baseline ตรงกับ §15.11 (base 7, tax 1.95×) */
    if (models[2].present)
        CHECK("X5: baseline Qwen2.5-0.5B → base 7 + tax 1.95× (ตรง §15.11)",
              models[2].base == 7 &&
              models[2].tax_real > 1.90 && models[2].tax_real < 2.00);

    printf("═══════════════════════════════════════════════════════════════\n");
    printf("models present: %u/3 — RESULTS: %d/%d PASS\n",
           n_present, pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
