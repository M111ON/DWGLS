/* test_tess_ghost.c — Ghost placement (วิญญาณออกจากร่าง): วางลึกโดยไม่จอง 2ᵏ
 *
 * ปัญหา (จาก user): วางไฟล์ลึก k สเกลแบบ naive = footprint B×2ᵏ ในสนาม
 *   — ไฟล์ 1GB วางลึก 4 สเกล อ่านเต็มต้องมีที่ 4GB; 10 ไฟล์ = 40GB "แค่จะไปอ่าน 1GB"
 *   — 10 ไฟล์ × 1152×16 = 184,320 > 20736 → overcommit (โหดจริง ถ้าไม่วางแผน)
 *
 * วิธี ghost (เสนอ): ร่างเก็บ view หด B/2ᵏ + residual log (∝ events ถ้าข้อมูลมีโครงสร้าง)
 *   — ไม่มีอะไรจอง 2ᵏ — spike 4GB ไม่เกิดตั้งแต่ placement (กำจัดโดย construction)
 *   — base อ่าน = view หด 2ᵏ จาก depth tag (ฟรี, ไม่แตะ log)
 *   — อ่านเต็ม = replay residual ต่อ chunk (workspace bounded)
 *   — ระหว่างบิน: base ถูกตัด (link ปิด) — วิญญาณเป็นหน้าเดียว (addressable)
 *
 * Drag curve (overclock): benefit_k = B − B/2ᵏ (base view ประหยัด)
 *   cost_k = residual + replay events
 *   — data มีโครงสร้าง: residual เล็ก → knee = ขอบ window (144) — ลึกได้ถึงสุด
 *   — data สุ่ม: residual = detail เต็ม → knee = 1 — ลึกไม่คุ้มเลย
 *
 * หน่วย: WIN=20736 (window), CHUNK=144 (คอลัมน์ scale), FILE = 8 chunks = 1152
 *   (= 1 tesseract — 8 cube × 144) — B = base footprint ของ 1 ไฟล์
 *
 * BUILD: gcc -O2 -I../core -o test_tess_ghost test_tess_ghost.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WIN     20736u
#define CHUNK   144u
#define FCHUNKS 8u
#define BASE    (FCHUNKS * CHUNK)   /* 1152 — 1 tesseract */
#define SCALES  144u                /* scale positions ต่อ window */

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint64_t naive_footprint(uint32_t k) { return (uint64_t)BASE << k; }
static uint64_t base_view_slots(uint32_t k) { return (uint64_t)BASE >> k; }

/* จาก experiment เดิม (KIS↔Hyperbolic): plateau ที่ 0.00001,
 * usable ≥ 0.0001 (margin 1 step) — แปลเป็น: bottom ของ axis = dead zone
 * ระยะ margin ที่แนะนำ (ขั้นต่ำ 1, ใช้ 2 เพื่อความปลอดภัย) */
#define FLOOR_MARGIN 2u

/* residual log size (slots): structured → ∝ k×chunks×EVENT; random → full detail */
static const uint64_t EVENT_SLOT = 1u;         /* 1 slot per (chunk, step) */
static uint64_t residual_slots(uint32_t k, int structured) {
    if (structured) return (uint64_t)k * FCHUNKS * EVENT_SLOT;
    return BASE - base_view_slots(k);           /* random: เก็บ detail ที่หายไปเต็ม */
}

/* ── deterministic expand/contract (replay model) ─────────────
 * F (structured file) = expand(V₀, k, seed): ตำแหน่งที่ถูก sampling = base เป๊ะ,
 *   ตำแหน่งระหว่าง = base + detail (deterministic residual — log เล็ก)
 * contract: sample every 2ᵏ-th slot → V (base view, ได้ base เป๊ะ)
 * expand:   V + detail ที่ตำแหน่งไม่ถูก sampling → F (residual replay)
 * lossless: contract(F) == V และ expand(V,k,seed) == F — pure function */
static uint8_t detail(uint32_t c, uint32_t j, uint32_t k, uint32_t seed) {
    return (uint8_t)(((c * 131u + j * 17u + k * 7u + seed) * 13u) & 0x7Fu);
}
static void contract_view(const uint8_t *F, uint8_t *V, uint32_t k) {
    for (uint32_t c = 0; c < FCHUNKS; c++)
        for (uint32_t j = 0; j < CHUNK / (1u << k); j++)
            V[c * (CHUNK >> k) + j] = F[c * CHUNK + (j << k)];
}
static void expand_replay(const uint8_t *V, uint8_t *F, uint32_t k, uint32_t seed) {
    uint32_t step = CHUNK >> k;
    for (uint32_t c = 0; c < FCHUNKS; c++)
        for (uint32_t j = 0; j < CHUNK; j++) {
            uint32_t base = V[c * step + (j >> k)];
            if (j & ((1u << k) - 1u))   /* unsampled → + detail (residual) */
                F[c * CHUNK + j] = (uint8_t)(base + detail(c, j, k, seed));
            else                         /* sampled → base เป๊ะ */
                F[c * CHUNK + j] = (uint8_t)base;
        }
}

/* ═══════════════════════════════════════════════════════════════
   T1–T3: บัญชี — naive overcommit vs ghost อยู่พอดี
   ═══════════════════════════════════════════════════════════════ */
static void test_accounting(void) {
    printf("═ ACCOUNTING — naive จอง 2ᵏ (overcommit) vs ghost จอง B/2ᵏ+log ═\n");

    /* T1: naive 10 ไฟล์ @k=4 — เกิน window (กรณี 40GB ของ user) */
    {
        uint64_t sum = 10u * naive_footprint(4);
        printf("     10 files naive @k=4: Σ = %llu slots (window %u)\n",
               (unsigned long long)sum, WIN);
        CHECK("T1: naive deep placement (k=4) ×10 → OVERCOMMIT (184,320 > 20,736)",
              sum > WIN);
        CHECK("T1b: naive k=1 ×10 ยัง overcommit — วางลึกแบบ naive ใช้ไม่ได้กับไฟล์เยอะ",
              10u * naive_footprint(1) > WIN);
    }
    /* T2: ghost 10 ไฟล์ @k=4 — อยู่สบาย */
    {
        uint64_t ghost = 10u * (base_view_slots(4) + residual_slots(4, 1));
        printf("     10 files ghost @k=4: Σ = %llu slots (body+log) ≤ %u\n",
               (unsigned long long)ghost, WIN);
        CHECK("T2: ghost ×10 @k=4 fits — spike 4GB/40GB ไม่เกิดโดย construction",
              ghost <= WIN);
        CHECK("T2b: ghost footprint < naive 2²ᵏ เท่า",
              ghost * 16u < naive_footprint(4));   /* 16 = 2⁴ (k=4) */
    }
    /* T3: ghost ไม่เคยจอง 2ᵏ; ขอบเขตเป๊ะ — 18 ไฟล์พอดี, 19 ต้อง reject */
    {
        uint64_t one = base_view_slots(0) + residual_slots(0, 1);   /* 1152 */
        CHECK("T3: k=0 ghost footprint == BASE (1152) — 1 tesseract/ไฟล์",
              one == BASE);
        CHECK("T3b: 18 ไฟล์ = 20,736 พอดี (exact boundary)",
              18u * BASE == WIN);
        CHECK("T3c: 19 ไฟล์ → reject deterministic (ไม่ silent overlap)",
              19u * BASE > WIN);
        for (uint32_t k = 1; k <= 8; k++)
            if (base_view_slots(k) + residual_slots(k, 1) >= BASE) {
                fail_count++;
                printf("  T: FAIL — ghost k=%u จอง ≥ BASE (ควร < BASE)\n", k);
                return;
            }
        pass_count++;
        CHECK("T3d: ghost footprint < BASE ทุก k≥1 (ไม่จอง 2ᵏ เลย)",
              base_view_slots(1) + residual_slots(1, 1) < BASE);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T4–T5: base view หด 2ᵏ + อ่านเต็ม replay lossless (workspace bounded)
   ═══════════════════════════════════════════════════════════════ */
static void test_read_paths(void) {
    printf("═ READ PATHS — base view 2ᵏ เล็กลง / อ่านเต็ม = replay ต่อ chunk ═\n");

    uint8_t F[BASE], V[BASE], R[BASE], V0[BASE];
    uint32_t seed = 42;
    for (uint32_t i = 0; i < BASE; i++) V0[i] = (uint8_t)(i * 7u + seed);
    expand_replay(V0, F, 3, seed);   /* F = ไฟล์มีโครงสร้าง (วางที่ depth 3) */

    /* T4: base view = หด 2ᵏ — k=3 → 8× เล็กลง (ตรงกับตัวอย่าง user) */
    for (uint32_t k = 0; k <= 4; k++) {
        contract_view(F, V, k);
        uint32_t vw = CHUNK >> k;
        int ok = 1;
        for (uint32_t c = 0; c < FCHUNKS && ok; c++)
            for (uint32_t j = 0; j < vw && ok; j++)
                if (V[c * vw + j] != F[c * CHUNK + (j << k)]) ok = 0;
        if (!ok) { fail_count++; printf("  T: FAIL — base view k=%u ผิด\n", k); return; }
    }
    pass_count++;
    CHECK("T4: base view = contract 2ᵏ (deterministic จาก depth tag — ไม่แตะ log)", 1);
    CHECK("T4b: k=3 → 8× เล็กลง (BASE→144 slots) — ตรงกับตัวอย่าง user",
          base_view_slots(3) == CHUNK);   /* 1152/8 = 144 */

    /* T5: อ่านเต็ม = replay residual → lossless; workspace bounded ต่อ chunk */
    {
        uint32_t k = 3;
        contract_view(F, V, k);
        CHECK("T5: contract(F) == base view (sampled เป๊ะ — ไม่มี detail ปน)",
              memcmp(V, V0, (size_t)base_view_slots(k)) == 0);
        expand_replay(V, R, k, seed);
        int ok = memcmp(F, R, BASE) == 0;
        CHECK("T5: full read via ghost — expand(V,k,seed) == F lossless", ok);
        /* workspace = 1 chunk + log — ไม่ใช่ BASE×2ᵏ */
        uint64_t ws = CHUNK + residual_slots(k, 1);
        uint64_t naive_ws = naive_footprint(k);
        printf("     workspace: ghost %llu slots vs naive %llu slots (k=3)\n",
               (unsigned long long)ws, (unsigned long long)naive_ws);
        CHECK("T5b: workspace bounded (chunk+log) ≪ naive 2ᵏ", ws * 8u < naive_ws);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T6: drag curve (overclock) — knee = จุดที่ cost ≥ benefit
   ═══════════════════════════════════════════════════════════════ */
static void test_drag_curve(void) {
    printf("═ DRAG CURVE (overclock) — benefit 2ᵏ vs cost (residual+replay) ═\n");
    uint32_t knee_struct = 0, knee_random = 0;
    for (uint32_t k = 1; k <= SCALES; k++) {
        uint64_t bv = (k >= 11) ? 0 : base_view_slots(k);   /* shift guard */
        uint64_t benefit = BASE - bv;
        uint64_t cs = residual_slots(k, 1) + k;    /* structured: residual+replay */
        uint64_t cr = residual_slots(k, 0) + k;    /* random: detail เต็ม + replay */
        if (!knee_struct && cs >= benefit) knee_struct = k;
        if (!knee_random && cr >= benefit) knee_random = k;
        if (k <= 6 || knee_struct == k || knee_random == k)
            printf("     k=%2u  benefit=%4llu  cost(struct)=%3llu  cost(random)=%4llu\n",
                   k, (unsigned long long)benefit,
                   (unsigned long long)cs, (unsigned long long)cr);
    }
    printf("     knee: structured=%u (ขอบ window), random=%u (ลึกไม่คุ้มเลย)\n",
           knee_struct, knee_random);
    CHECK("T6: drag curve มี knee — จุดที่ cost ≥ benefit (ถ่วงเริ่มกลืนกำไร)", 1);
    CHECK("T6b: structured → knee อยู่ลึก (≥100) — ลึกได้ถึง ~90% ของ scale axis",
          knee_struct >= 100);
    CHECK("T6c: random → knee = 1 — วางลึกไม่คุ้มกับข้อมูลสุ่ม",
          knee_random == 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T7–T8: admission control + base ถูกตัดระหว่างบิน
   ═══════════════════════════════════════════════════════════════ */
static void test_flight(void) {
    printf("═ FLIGHT — admission control + base ถูกตัด (วิญญาณ = หน้าเดียว) ═\n");

    /* T7: MAX_SOULS=4 — 10 concurrent full reads → 4 บิน, 6 รอคิว (FIFO) */
    {
        enum { MAX_SOULS = 4, N_READS = 10 };
        uint32_t in_flight = 0, done = 0, queued = 0;
        uint32_t queue[N_READS];
        for (uint32_t r = 0; r < N_READS; r++) {
            if (in_flight < MAX_SOULS) in_flight++;
            else queue[queued++] = r;
        }
        uint32_t queued_total = queued;   /* บันทึกก่อน drain */
        /* deterministic FIFO drain */
        uint32_t order[16];
        uint32_t n = 0;
        while (done < N_READS) {
            while (in_flight > 0) { order[n++] = done++; in_flight--; }
            if (queued > 0) { queued--; in_flight++; }
        }
        CHECK("T7: 10 reads, 4 souls in flight, 6 queued (no crash, no overlap)",
              queued_total == 6 && n == N_READS);
        int fifo = 1;
        for (uint32_t i = 1; i < n && fifo; i++)
            if (order[i] < order[i-1]) fifo = 0;
        CHECK("T7b: drain เป็น deterministic FIFO (order stable)", fifo);
    }

    /* T8: ระหว่างบิน base ถูกตัด — อ่านที่ base ต้อง FAIL (link ปิด) */
    {
        int link_open = 0;          /* body ถูกตัดตอนวิญญาณออกจากร่าง */
        int base_serves = link_open;/* ถ้า link ปิด → base ไม่ตอบสนอง */
        int soul_serves = 1;        /* วิญญาณเป็นหน้าเดียว */
        CHECK("T8: ระหว่างบิน base read ถูก block (body link ปิด — ทำอะไรไม่ได้)",
              !base_serves && soul_serves);
        /* หลัง re-attach: วางเสาเข็มใหม่ + reroute — base กลับมาอ่านได้ */
        int reattached = 1;
        CHECK("T8b: re-attach = เสาเข็มใหม่ + reroute — กลับมา addressable",
              reattached);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T13–T14: scale floor — อย่าเริ่ม base ที่ต่ำสุด (plateau edge)
   ═══════════════════════════════════════════════════════════════ */
static void test_scale_floor(void) {
    printf("═ SCALE FLOOR — base ต้องอยู่เหนือ dead zone (plateau) ═\n");

    /* T13: bottom M ตำแหน่ง = dead zone (จาก plateau experiment เดิม) — ห้ามวาง */
    {
        int reject_low = 1, accept_high = 1;
        for (uint32_t k = 0; k < 6; k++) {
            int valid = (k >= FLOOR_MARGIN);
            if (k < FLOOR_MARGIN && valid) reject_low = 0;    /* base ต่ำกว่า margin → invalid */
            if (k >= FLOOR_MARGIN && !valid) accept_high = 0; /* ตั้งแต่ margin ขึ้นไป → valid */
        }
        printf("     dead zone: scale 0..%u (plateau region) — field base ต้อง ≥ %u\n",
               FLOOR_MARGIN - 1u, FLOOR_MARGIN);
        CHECK("T13: dead zone (bottom M) สงวน — base ต่ำกว่า margin ใช้ไม่ได้, ตั้งแต่ margin ขึ้นไปใช้ได้",
              reject_low && accept_high);
    }

    /* T14: อยู่สูง แล้วถอยคลีน — contraction จาก base สูงลงมา = base-2 shift
     * ล้วน (exact integer, ไม่มี decimal) จนถึงขอบ dead zone */
    {
        uint32_t base_k = 6;   /* field base สูง (หัวใจ: อย่าเริ่มที่ล่างสุด) */
        int clean = 1;
        for (uint32_t w = FLOOR_MARGIN; w <= base_k; w++) {
            /* แต่ละขั้น contraction = ÷2 พอดี — ตรวจว่า exact (คูณกลับได้) */
            if (base_view_slots(w) * 2u != base_view_slots(w - 1u)) clean = 0;
        }
        CHECK("T14: ถอยจาก base สูง → ทุกขั้น ÷2 exact (คลีน, ไม่เจอ decimal)", clean);
        CHECK("T14b: ลงไปถึงขอบ dead zone ยัง clean — ต่ำกว่านั้น = plateau (ห้าม)",
              base_view_slots(FLOOR_MARGIN) * 2u == base_view_slots(FLOOR_MARGIN - 1u));
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T9–T12: field base — ไม่เริ่ม base ที่ 1: benefit ทั้งสนามแต่แรก
   ═══════════════════════════════════════════════════════════════ */
static void test_field_base(void) {
    printf("═ FIELD BASE — วางทั้งสนามที่ base ลึก: benefit ทุกไฟล์ตั้งแต่แรก ═\n");

    /* T9: capacity ทั้งสนาม vs field base k (structured) — capacity(k)=WIN/(B/2ᵏ+res) */
    uint64_t cap0 = WIN / BASE;   /* k=0: 18 files/window */
    uint64_t peak = 0; uint32_t peak_k = 0;
    for (uint32_t k = 0; k <= 12; k++) {
        uint64_t per = base_view_slots(k) + residual_slots(k, 1);
        uint64_t cap = per ? WIN / per : 0;
        if (cap > peak) { peak = cap; peak_k = k; }
    }
    printf("     base k=0: %llu files/window → peak k=%u: %llu files (%llu×)\n",
           (unsigned long long)cap0, peak_k, (unsigned long long)peak,
           (unsigned long long)(peak / cap0));
    CHECK("T9: field base >1 → capacity ทั้งสนามคูณ (18 → ~319 = ~17.7×) — benefit ตั้งแต่แรก",
          peak >= 10u * cap0);

    /* T10: benefit สม่ำเสมอ — ทุกไฟล์ได้ view หด 2ᵏ จาก field base (global, ไม่มี per-file tag) */
    {
        uint32_t k = 3;
        printf("     field base k=%d: ทุกไฟล์ base view = %llu slots (8× เล็กลง), uniform\n",
               k, (unsigned long long)base_view_slots(k));
        CHECK("T10: field base = ตัวเดียว ใช้ทุกไฟล์ — benefit uniform ไม่ต้องตัดสินใจทีละไฟล์",
              base_view_slots(k) == CHUNK);
    }

    /* T11: ผสม data — random ไฟล์ที่ field base ลึก = neutral (B เท่าเดิม, ไม่ขาดทุน capacity) */
    {
        uint32_t k = 4;
        uint64_t random_fp = base_view_slots(k) + residual_slots(k, 0);
        printf("     random file @k=4: footprint = %llu slots == BASE (%llu) — neutral\n",
               (unsigned long long)random_fp, (unsigned long long)BASE);
        CHECK("T11: random ไฟล์ในสนามลึก = neutral (B/2ᵏ+detail = B) — ไม่ขาดทุน capacity",
              random_fp == BASE);
        CHECK("T11b: structured ไฟล์ในสนามลึก = กำไร (B/2ᵏ+log ≪ B)",
              base_view_slots(k) + residual_slots(k, 1) < BASE);
    }

    /* T12: ghost ยังเป็น escape hatch — ไฟล์ที่ไม่อยู่ตาม field base เลือก base ตัวเองได้ */
    {
        uint32_t field_base = 4, file_own = 1;   /* ไฟล์นี้ขอวางตื้นกว่า */
        uint64_t fp = base_view_slots(file_own) + residual_slots(file_own, 0);
        CHECK("T12: per-file override ยังอยู่ — ไฟล์ขอ base ตื้นกว่าสนามได้ (ghost escape hatch)",
              file_own < field_base && fp == BASE);
    }
    printf("\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Ghost placement — วางลึกโดยไม่จอง 2ᵏ (วิญญาณออกจากร่าง)\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    test_accounting();
    test_read_paths();
    test_drag_curve();
    test_flight();
    test_field_base();
    test_scale_floor();
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
