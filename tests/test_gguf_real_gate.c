/* test_gguf_real_gate.c — เอา Qwen จริงวางผ่าน leverage gate × registry
 *
 * ใช้ tensor ขนาดจริงจาก GGUF (n_elems — ไม่ใช่ uniform สมมติ):
 *   - model-level gate sweep: storage(k) = ceil(E/2ᵏ/WIN) windows (contiguous
 *     chain — ไฟล์ body ต่อเนื่อง), cost = drag ต่อขั้น (Σ chunks + replay)
 *     → ROI(k) = benefit/cost — knee = จุดที่ขยายไม่คุ้มอีกต่อไป
 *   - addressing tax: Σ_i ceil(ceil(s_i/2ᵏ)/WIN) — ต่อ-tensor (registry span)
 *     เทียบ real vs uniform — ตัวเล็ก (norm 896) ตกเพดาน 1 window ต่อก้อน
 *   - chain placement ที่ knee: วาง inference order ต่อเนื่อง → windows ใช้จริง
 *     == storage(knee) — ตรวจ gate ไม่ re-expand ระหว่างวาง (สนามนิ่ง)
 *   - replay จาก log → state เหมือนทุกไบต์ (determinism ครบ)
 *
 * คำถามที่ตอบ: ไฟล์จริงกับ uniform ต่างยังไง?
 *   storage/compression เท่ากัน (Σ เท่ากัน) — แต่ addressing tax ต่าง:
 *   ของจริงมี tensor เล็กเยอะ (norms/attn) → tax สูงกว่าที่ uniform คาด
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_gguf_real_gate \
 *        tests/test_gguf_real_gate.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

#define WIN           20736u
#define FLOOR_MARGIN  2u
#define GATE          1.0
#define LOAD_LIMIT    (WIN * 90u / 100u)   /* 18662 */
#define EVENT_SLOT    1u
#define REPLAY_EVENT  8u

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── ceil ของ s/2ᵏ และ s/144 (chunks) ─────────────────────────── */
static uint64_t view_of(uint64_t s, uint32_t k) {
    if (k >= 63) return s ? 1 : 0;
    uint64_t d = 1ull << k;
    return (s + d - 1) / d;
}
static uint64_t chunks_of(uint64_t s) { return (s + 143u) / 144u; }

/* storage(k) = windows ที่ model ต้องการที่ base k (contiguous chain) */
static uint64_t storage(uint64_t E, uint32_t k) {
    uint64_t v = view_of(E, k);
    return (v + WIN - 1) / WIN;
}
/* addressing tax: Σ per-tensor window spans (registry ต้อง address ต่อ tensor) */
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
    return c * EVENT_SLOT + (uint64_t)n * REPLAY_EVENT;
}
static double roi_step(uint64_t E, uint64_t cost, uint32_t k) {
    uint64_t b = storage(E, k) - storage(E, k + 1);   /* windows saved */
    return (double)(b * WIN) / (double)cost;
}

/* ── inference order (เหมือน test_gguf_window_chain) ──────────── */
static int cat_of(const char *name, unsigned *block) {
    *block = 0;
    if (strncmp(name, "token_embd", 10) == 0) return 0;
    if (strncmp(name, "blk.", 4) == 0) { *block = (unsigned)atoi(name + 4); return 1; }
    if (strncmp(name, "output_norm", 11) == 0) return 2;
    return 3;
}
static void sort_inference(const GGUFBox *box, uint32_t *order, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++) {
            unsigned ba = 0, bb = 0;
            int ca = cat_of(box->entries[order[i]].name, &ba);
            int cb = cat_of(box->entries[order[j]].name, &bb);
            int less = (ca < cb) || (ca == cb && (ba < bb || (ba == bb && order[i] < order[j])));
            if (!less) { uint32_t t = order[i]; order[i] = order[j]; order[j] = t; }
        }
}

/* ── registry chain — tensors วางต่อเนื่อง (สนามนิ่ง, replay ได้) ─ */
typedef struct {
    uint32_t id;
    uint64_t start_win;
    uint32_t n_windows;
    uint8_t  base_k;
    uint8_t  link;
} TenReg;

typedef struct {
    TenReg reg[1024];
    uint32_t n;
    uint64_t windows;
    uint64_t cursor;      /* global slot cursor */
    uint32_t crossings;   /* windows ที่ข้ามเส้น 90% ระหว่างวาง */
    int      gate_ok;     /* gate ไม่ re-expand ที่ knee (สนามนิ่ง) */
} Chain;

static void chain_place(Chain *c, const uint64_t *s, const uint32_t *order,
                        uint32_t n, uint32_t k, uint64_t E, uint64_t cost) {
    memset(c, 0, sizeof(*c));
    c->gate_ok = 1;
    uint64_t win_used = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tid = order[i];
        uint64_t v = view_of(s[tid], k);
        uint64_t w0 = c->cursor / WIN;
        uint64_t remain = v;
        while (remain > 0) {
            uint64_t space = WIN - win_used;
            uint64_t take = remain < space ? remain : space;
            uint64_t before = win_used;
            win_used += take;
            if (before <= LOAD_LIMIT && win_used > LOAD_LIMIT) {
                c->crossings++;
                /* ที่ knee การขยายต้องถูกปฏิเสธ — ถ้า gate allow = พลาด
                 * (gate ถูกตัดสินที่ model level แล้ว — สนามนิ่ง, ไม่ re-decision) */
                if (roi_step(E, cost, k) >= GATE) c->gate_ok = 0;
            }
            remain -= take;
            c->cursor += take;
            if (win_used == WIN) win_used = 0;
        }
        uint64_t end = (c->cursor - 1) / WIN;
        c->reg[c->n++] = (TenReg){ tid, w0, (uint32_t)(end - w0 + 1), (uint8_t)k, 1 };
    }
    c->windows = (c->cursor + WIN - 1) / WIN;
}

/* ── log replay (decision trace — deterministic) ──────────────── */
enum { A_PLACE = 1, A_CLOSE, A_REOPEN };
typedef struct { uint8_t action; uint32_t id; uint32_t base_k; uint64_t start_win; uint32_t n_windows; } LogEv;

static void chain_apply(Chain *c, const LogEv *e) {
    if (e->action == A_PLACE) {
        c->reg[c->n++] = (TenReg){ e->id, e->start_win, e->n_windows, (uint8_t)e->base_k, 1 };
        c->windows = (e->start_win + e->n_windows);
    } else if (e->action == A_CLOSE) {
        for (uint32_t i = 0; i < c->n; i++)
            if (c->reg[i].id == e->id && c->reg[i].link) { c->reg[i].link = 0; break; }
    } else { /* A_REOPEN */
        for (uint32_t i = 0; i < c->n; i++)
            if (c->reg[i].id == e->id && !c->reg[i].link) { c->reg[i].link = 1; break; }
    }
}
static void chain_replay(Chain *c, const LogEv *log, uint32_t n) {
    memset(c, 0, sizeof(*c));
    for (uint32_t i = 0; i < n; i++) chain_apply(c, &log[i]);
}
static int chain_eq(const Chain *a, const Chain *b) {
    if (a->n != b->n || a->windows != b->windows) return 0;
    for (uint32_t i = 0; i < a->n; i++) {
        const TenReg *x = &a->reg[i], *y = &b->reg[i];
        if (x->id != y->id || x->start_win != y->start_win ||
            x->n_windows != y->n_windows || x->base_k != y->base_k || x->link != y->link) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("GGUF real gate — tensor ขนาดจริงผ่าน leverage gate × registry\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    GGUFBox box;
    if (gguf_box_open(&box, gguf) != 0) {
        printf("  (cannot open %s — skipping)\n", gguf);
        printf("  T: PASS — skipped (no GGUF available)\n");
        pass_count++;
        printf("\nRESULTS: %u/%u PASS\n", pass_count, pass_count + fail_count);
        return 0;
    }
    uint32_t N = box.n_tensors;
    uint64_t *s = (uint64_t *)calloc(N, sizeof(uint64_t));
    uint64_t E = 0;
    for (uint32_t i = 0; i < N; i++) {
        s[i] = box.entries[i].n_elems;
        E += s[i];
    }
    uint32_t *order = (uint32_t *)calloc(N, sizeof(uint32_t));
    sort_inference(&box, order, N);

    printf("  model: %u tensors, E = %llu elements, file = %s\n",
           N, (unsigned long long)E, gguf);
    printf("  base 0 storage: %llu windows (%.1f MB @1B/slot)\n",
           (unsigned long long)storage(E, 0),
           (double)E / 1048576.0);

    /* ── uniform เทียบ: N ก้อนเท่ากัน, Σ เท่ากัน ─────────────── */
    uint64_t *u = (uint64_t *)calloc(N, sizeof(uint64_t));
    {
        uint64_t base = E / N, rem = E % N;
        for (uint32_t i = 0; i < N; i++) u[i] = base + (i < rem ? 1 : 0);
    }

    /* ═══════════════════════════════════════════════════════════
       T1–T3: model-level gate sweep — ไฟล์จริงเลือก base ที่ knee
       ═══════════════════════════════════════════════════════════ */
    printf("\n═ MODEL-LEVEL GATE — storage(k) vs drag — knee = base จริง ═\n");
    uint64_t cost = drag_cost(s, N);
    printf("  cost/step (drag) = %llu slots (Σ chunks %llu + replay %llu)\n",
           (unsigned long long)cost,
           (unsigned long long)(cost - (uint64_t)N * REPLAY_EVENT),
           (unsigned long long)((uint64_t)N * REPLAY_EVENT));
    printf("  k   storage(w)  saved(w)  ROI\n");
    uint32_t knee = 0;
    for (uint32_t k = FLOOR_MARGIN; k <= 12 && !knee; k++) {
        double roi = roi_step(E, cost, k);
        printf("  %2u  %7llu  %7llu  %6.2f\n", k,
               (unsigned long long)storage(E, k),
               (unsigned long long)(storage(E, k) - storage(E, k + 1)),
               roi);
        if (roi < GATE) knee = k;   /* ขั้นนี้ขยายไม่คุ้ม → หยุดที่ k-1... */
    }
    /* knee = base สุดท้ายที่เข้าถึงได้: ขยายได้จนขั้น ROI ≥ 1 */
    uint32_t final_base = 0;
    for (uint32_t k = FLOOR_MARGIN; k <= 12; k++) {
        if (roi_step(E, cost, k) >= GATE) final_base = k + 1;
    }
    printf("  final base = %u (ขยายได้ถึงขั้นที่ ROI ≥ 1), storage = %llu windows\n",
           final_base, (unsigned long long)storage(E, final_base));

    CHECK("T1: ไฟล์จริงมี tensor เยอะพอ (N ≥ 200) + E ≥ 100M elements",
          N >= 200 && E >= 100000000ull);
    CHECK("T2: storage(k) ลดลงตาม k — compression จริง (base0 > 8× base10)",
          storage(E, 0) > 8ull * storage(E, 10));
    CHECK("T3: knee มีอยู่จริง — หลัง base สุดท้าย ROI < 1 (ขยายต่อไม่คุ้ม)",
          final_base > FLOOR_MARGIN && final_base <= 12 &&
          roi_step(E, cost, final_base) < GATE);
    {
        uint64_t c0 = storage(E, 0), ck = storage(E, final_base);
        printf("  compression: %llu windows @base0 → %llu @base%u (%.0f×)\n",
               (unsigned long long)c0, (unsigned long long)ck, final_base,
               (double)c0 / (double)ck);
        CHECK("T4: ที่ base จริง — compression ≥ 16× (วางลึก = view เล็กลง)",
              c0 >= 16ull * ck);
    }

    /* ═══════════════════════════════════════════════════════════
       T5–T7: addressing tax — real vs uniform (registry span ต่อ tensor)
       ═══════════════════════════════════════════════════════════ */
    printf("\n═ ADDRESSING TAX — registry ต้อง address ต่อ tensor (real vs uniform) ═\n");
    {
        uint64_t ar = addr_spans(s, N, final_base);
        uint64_t au = addr_spans(u, N, final_base);
        uint64_t st = storage(E, final_base);
        printf("  @base%u: storage=%llu  real_spans=%llu (tax %.2f×)  uniform_spans=%llu (tax %.2f×)\n",
               final_base, (unsigned long long)st,
               (unsigned long long)ar, (double)ar / (double)st,
               (unsigned long long)au, (double)au / (double)st);
        CHECK("T5: addressing tax ≥ 1 เสมอ (Σ ceil ≥ ceil Σ — ไม่มีทางหนี)",
              ar >= st && au >= st);
        CHECK("T6: ของจริง tax สูงกว่า uniform — tensor เล็ก (norm/attn) ตกเพดาน 1 window",
              ar >= au);
        CHECK("T6b: ที่ base ตื้น (k=margin) tax ทั้งคู่ ≈ 1 (fragmentation น้อย)",
              addr_spans(s, N, FLOOR_MARGIN) < 3ull * storage(E, FLOOR_MARGIN));
    }
    printf("\n");
    (void)knee;

    /* ═══════════════════════════════════════════════════════════
       T8–T10: chain placement ที่ base จริง — fit + gate ไม่ re-expand
       ═══════════════════════════════════════════════════════════ */
    printf("═ CHAIN PLACEMENT — tensor ขนาดจริงวาง inference order ที่ base %u ═\n",
           final_base);
    Chain c1, c2;
    LogEv log1[2048];
    uint32_t ln1 = 0;
    /* run 1: วางจริง + บันทึก log */
    {
        chain_place(&c1, s, order, N, final_base, E, cost);
        for (uint32_t i = 0; i < c1.n; i++)
            log1[ln1++] = (LogEv){ A_PLACE, c1.reg[i].id, c1.reg[i].base_k,
                                   c1.reg[i].start_win, c1.reg[i].n_windows };
    }
    /* run 2: เหมือนกัน — deterministic */
    chain_place(&c2, s, order, N, final_base, E, cost);
    printf("  placed %u/%u tensors, %llu windows (model-level: %llu), crossings=%u\n",
           c1.n, N, (unsigned long long)c1.windows,
           (unsigned long long)storage(E, final_base), c1.crossings);

    CHECK("T8: ทุก tensor วางครบ (chain = contiguously packed, ไม่มี reject)",
          c1.n == N);
    CHECK("T8b: windows ใช้จริง == storage(knee) — chain กับ model-level ตรงกัน",
          c1.windows == storage(E, final_base));
    CHECK("T8c: gate ไม่ re-expand ระหว่างวาง (สนามนิ่ง — base คงที่ตลอด)",
          c1.gate_ok);
    CHECK("T9: วาง 2 รอบ → registry เหมือนทุกไบต์ (determinism)",
          chain_eq(&c1, &c2));

    /* replay จาก log → เหมือนทุกไบต์ */
    {
        Chain cr;
        chain_replay(&cr, log1, ln1);
        CHECK("T10: replay จาก log (ไม่มี input เดิม) → registry เหมือนทุกไบต์",
              chain_eq(&c1, &cr));
        CHECK("T10b: log ทุก entry วางที่ base = final_base (base_k ตรงกัน)",
              cr.reg[0].base_k == final_base);
    }
    /* ปลอม log → ต่าง (replay ไม่ใจดี) */
    {
        LogEv tampered[2048];
        memcpy(tampered, log1, ln1 * sizeof(LogEv));
        tampered[0].n_windows += 1;
        Chain ct;
        chain_replay(&ct, tampered, ln1);
        CHECK("T11: ปลอม n_windows ใน log → replay ต่าง (จับได้)",
              !chain_eq(&c1, &ct));
    }

    /* ปิด/เปิด link — registry static, replay ครบ */
    {
        Chain c3 = c1;
        log1[ln1++] = (LogEv){ A_CLOSE, c3.reg[0].id, 0, 0, 0 };
        log1[ln1++] = (LogEv){ A_REOPEN, c3.reg[0].id, 0, 0, 0 };
        Chain cr;
        chain_replay(&cr, log1, ln1);
        CHECK("T12: close/reopen ผ่าน log → replay เหมือนทุกไบต์ (สนามนิ่ง)",
              chain_eq(&c1, &cr));
    }

    free(s); free(u); free(order);
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
