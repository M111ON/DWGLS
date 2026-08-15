/* test_tess_registry_gate.c — Leverage gate ต่อ registry (placement ควบคุมด้วย ROI)
 *
 * วางไฟล์ → ถ้า load จะข้ามเส้น 90% ของ window → gate ตัดสินใจ:
 *    ขยาย field base (k+1) ถ้า ROI ≥ GATE   หรือ   ปฏิเสธ (deterministic)
 * ทั้งหมดเป็น pure function ของลำดับ input → replay จาก log ได้ state เหมือนเดิมทุกไบต์
 *
 * registry = fixed array (สนามนิ่ง): entry ไม่ขยับ, addr ไม่เปลี่ยน
 *   ปิด link (close) = คืน load แต่เสาเข็มยังอยู่ → เปิดใหม่ addr เดิม, data เดิม
 *   (ตรงกับ §5 เสาเข็ม / §7 link reroute — "ไม่มีอะไรถูกย้าย")
 *
 * ตัวเลข (ต่อจาก test_tess_ghost / test_tess_leverage):
 *   B=1152 (1 tesseract/ไฟล์), fp(k)=B/2ᵏ+8k, capacity(k)=20736/fp(k)
 *   ROI(k) จาก leverage gate: 8.49→4.02→1.74→0.64→0.06 (k=2..6), knee = 5
 *   WIN=20736, เส้น 90% = 18662 — trigger ทุกครั้งที่ placement ข้ามเส้น
 *
 * สถานะคาดหวัง scenario A (100 ไฟล์ uniform, GATE=1.0):
 *   files 1..61 @k=2 (304 ต่อไฟล์) → load 18544 (89.4%)
 *   file 62: ข้ามเส้น → ROI(2)=8.49 → ขยาย k=3, วางที่ fp(3)=168
 *   file 63: ข้าม → ROI(3)=4.02 → ขยาย k=4, วาง fp(4)=104
 *   file 64: ข้าม → ROI(4)=1.74 → ขยาย k=5, วาง fp(5)=76
 *   file 65+: ROI(5)=0.64 < 1 → ปฏิเสธทั้งหมด
 *   สุดท้าย: 64 วาง, 36 ปฏิเสธ, field_base=5, load=18892 (91.1%) — ไม่เคยเกิน WIN
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format \
 *        -o tests/test_tess_registry_gate tests/test_tess_registry_gate.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define WIN          20736u
#define CHUNK        144u
#define FCHUNKS      8u
#define BASE         (FCHUNKS * CHUNK)    /* 1152 */
#define FLOOR_MARGIN 2u
#define EVENT_SLOT   1u
#define REPLAY_EVENT FCHUNKS
#define LOAD_PCT     90u
#define LOAD_LIMIT   (WIN * LOAD_PCT / 100u)   /* 18662 */
#define GATE         1.0
#define REG_SLOTS    256u
#define LOG_MAX      2048u

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── ตัวแบบจาก leverage gate (เลขชุดเดียวกัน) ─────────────────── */
static uint64_t view(uint32_t k)      { return (k >= 11) ? 0 : (uint64_t)BASE >> k; }
static uint64_t residual(uint32_t k)  { return (uint64_t)k * FCHUNKS * EVENT_SLOT; }
static uint64_t fp(uint32_t k)        { return view(k) + residual(k); }
static uint64_t capacity(uint32_t k)  { uint64_t f = fp(k); return f ? WIN / f : 0; }
static int64_t  dcap(uint32_t k)      { return (int64_t)capacity(k + 1) - (int64_t)capacity(k); }
static uint64_t dcost_per_file(void)  { return residual(1) + REPLAY_EVENT; }
static double roi_field(uint32_t k) {
    if (dcap(k) <= 0) return -1.0;
    return (double)dcap(k) * (double)fp(k + 1) /
           ((double)capacity(k) * (double)dcost_per_file());
}
static int gate_allow(uint32_t k, double gate, int pressure, double *roi_out) {
    if (roi_out) *roi_out = roi_field(k);
    if (!pressure) return 0;
    if (dcap(k) <= 0) return 0;
    if (roi_field(k) < gate) return 0;
    return 1;
}

/* ── Registry (fixed array — สนามนิ่ง) ────────────────────────── */
typedef struct {
    uint32_t file_id;
    uint16_t addr;       /* address ใน window — 2 ไบต์ */
    uint8_t  base_k;     /* scale ที่ land */
    uint16_t n_slots;    /* footprint fp(base_k) */
    uint8_t  link;       /* 1 = เปิด (active), 0 = ปิด (คืน load) */
} RegEntry;

typedef struct {
    RegEntry reg[REG_SLOTS];
    uint32_t n;          /* จำนวน entry (append-only, ไม่เคยขยับ) */
    uint32_t load;       /* slots ที่ใช้จริง (ผลรวม n_slots ของ link เปิด) */
    uint32_t field_base; /* global base ของสนาม */
    uint32_t rejected;   /* จำนวนที่ gate ปฏิเสธ */
} Field;

/* ── Log (decision trace — replay ได้) ────────────────────────── */
enum { ACT_PLACE = 1, ACT_REJECT, ACT_EXPAND, ACT_CLOSE, ACT_REOPEN };
typedef struct {
    uint32_t file_id;
    uint8_t  action;
    uint8_t  base_k;     /* PLACE/REJECT: k ที่เกี่ยวข้อง; EXPAND: k เก่าที่ถูก gate */
    uint16_t addr;       /* PLACE: address ที่วาง */
} LogEv;

static void log_push(LogEv *log, uint32_t *log_n, uint8_t act, uint32_t fid,
                     uint32_t k, uint32_t addr) {
    log[*log_n] = (LogEv){ fid, act, (uint8_t)k, (uint16_t)addr };
    (*log_n)++;
}

/* ── placement — gate ตัดสินใจที่เส้น 90% ─────────────────────── */
enum { R_OK = 1, R_REJ = 0 };
static int field_place(Field *f, uint32_t file_id, LogEv *log, uint32_t *log_n) {
    uint32_t k = f->field_base;
    uint64_t n = fp(k);
    uint64_t la = (uint64_t)f->load + n;
    if (la <= LOAD_LIMIT) {                        /* ปกติ: ไม่ข้ามเส้น */
        if (la > WIN) return R_REJ;                /* guard — ล้น window */
        f->reg[f->n] = (RegEntry){ file_id, (uint16_t)f->load, (uint8_t)k,
                                   (uint16_t)n, 1 };
        log_push(log, log_n, ACT_PLACE, file_id, k, f->load);
        f->load += n; f->n++;
        return R_OK;
    }
    /* ข้ามเส้น 90% → gate ตัดสินใจ: ขยาย (ROI ≥ GATE) หรือปฏิเสธ */
    double roi = 0;
    if (gate_allow(k, GATE, 1, &roi)) {
        uint32_t k2 = k + 1;
        uint64_t n2 = fp(k2);
        if ((uint64_t)f->load + n2 > WIN) return R_REJ;  /* ขยายแล้วยังล้น → ปฏิเสธ */
        log_push(log, log_n, ACT_EXPAND, 0, k, 0);       /* k เก่าที่ถูก gate */
        f->field_base = k2;
        f->reg[f->n] = (RegEntry){ file_id, (uint16_t)f->load, (uint8_t)k2,
                                   (uint16_t)n2, 1 };
        log_push(log, log_n, ACT_PLACE, file_id, k2, f->load);
        f->load += n2; f->n++;
        return R_OK;
    }
    log_push(log, log_n, ACT_REJECT, file_id, k, 0);
    f->rejected++;
    return R_REJ;
}

/* ปิด/เปิด link — เสาเข็มอยู่, แค่คืน/จอง load */
static void field_close(Field *f, uint32_t file_id, LogEv *log, uint32_t *log_n) {
    for (uint32_t i = 0; i < f->n; i++)
        if (f->reg[i].file_id == file_id && f->reg[i].link) {
            f->reg[i].link = 0;
            f->load -= f->reg[i].n_slots;
            log_push(log, log_n, ACT_CLOSE, file_id, 0, 0);
            return;
        }
}
static void field_reopen(Field *f, uint32_t file_id, LogEv *log, uint32_t *log_n) {
    for (uint32_t i = 0; i < f->n; i++)
        if (f->reg[i].file_id == file_id && !f->reg[i].link) {
            f->reg[i].link = 1;
            f->load += f->reg[i].n_slots;
            log_push(log, log_n, ACT_REOPEN, file_id, 0, 0);
            return;
        }
}

/* ── replay — apply decision trace ต่อ state เปล่า ────────────── */
static void field_apply(Field *f, const LogEv *e) {
    switch (e->action) {
    case ACT_EXPAND: f->field_base = e->base_k + 1; break;
    case ACT_PLACE: {
        uint16_t n = (uint16_t)fp(e->base_k);        /* คำนวณใหม่จาก k (จับการปลอม) */
        f->reg[f->n] = (RegEntry){ e->file_id, e->addr, e->base_k, n, 1 };
        f->n++; f->load += n;
        break;
    }
    case ACT_REJECT: f->rejected++; break;
    case ACT_CLOSE:
        for (uint32_t i = 0; i < f->n; i++)
            if (f->reg[i].file_id == e->file_id && f->reg[i].link) {
                f->reg[i].link = 0; f->load -= f->reg[i].n_slots; break;
            }
        break;
    case ACT_REOPEN:
        for (uint32_t i = 0; i < f->n; i++)
            if (f->reg[i].file_id == e->file_id && !f->reg[i].link) {
                f->reg[i].link = 1; f->load += f->reg[i].n_slots; break;
            }
        break;
    }
}
static void field_replay(Field *f, const LogEv *log, uint32_t log_n) {
    memset(f, 0, sizeof(*f));
    f->field_base = FLOOR_MARGIN;   /* ค่าคงที่ตอนสร้างสนาม (อย่าเริ่มที่ต่ำสุด — §15.7) */
    for (uint32_t i = 0; i < log_n; i++) field_apply(f, &log[i]);
}

static int field_eq(const Field *a, const Field *b) {
    if (a->n != b->n || a->load != b->load || a->field_base != b->field_base ||
        a->rejected != b->rejected) return 0;
    for (uint32_t i = 0; i < a->n; i++) {
        const RegEntry *x = &a->reg[i], *y = &b->reg[i];
        if (x->file_id != y->file_id || x->addr != y->addr || x->base_k != y->base_k ||
            x->n_slots != y->n_slots || x->link != y->link) return 0;
    }
    return 1;
}

/* ── scenario A: 100 ไฟล์ uniform → gate นำพา 64 วาง / 36 ปฏิเสธ ── */
static void run_scenario_a(Field *f, LogEv *log, uint32_t *log_n) {
    memset(f, 0, sizeof(*f));
    f->field_base = FLOOR_MARGIN;   /* field base เริ่มที่ขอบ dead zone (ROI(2)=8.49 — ขยายก้าวแรกคุ้ม) */
    *log_n = 0;
    for (uint32_t i = 1; i <= 100; i++) field_place(f, i, log, log_n);
}

/* ═══════════════════════════════════════════════════════════════
   T1–T3: gate ทำงาน + determinism — 2 รอบ ได้ state เดียวกัน
   ═══════════════════════════════════════════════════════════════ */
static void test_gate_behavior(void) {
    printf("═ GATE + 90% TRIGGER — placement ถูกควบคุมด้วย ROI อย่างเดียว ═\n");

    Field f1, f2;
    LogEv log1[LOG_MAX], log2[LOG_MAX];
    uint32_t n1 = 0, n2 = 0;
    run_scenario_a(&f1, log1, &n1);
    run_scenario_a(&f2, log2, &n2);

    printf("     final: placed=%u rejected=%u field_base=%u load=%u/%u (%.1f%%)\n",
           f1.n, f1.rejected, f1.field_base, f1.load, WIN,
           100.0 * f1.load / WIN);

    /* T1: gate นำพาไปถูกที่ — 64 วาง, 36 ปฏิเสธ, base=5, load=18892 */
    CHECK("T1: 64 placed / 36 rejected / field_base=5 (knee) / load=18892",
          f1.n == 64 && f1.rejected == 36 && f1.field_base == 5 && f1.load == 18892);
    CHECK("T1b: ก่อนข้ามเส้น load ≤ 90% (ไฟล์ 61 แรก = 18544 ≤ 18662)",
          61u * fp(2) == 18544 && 61u * fp(2) <= LOAD_LIMIT);
    CHECK("T1c: load ไม่เคยเกิน WIN (20736) — เส้น 90% เป็น trigger ไม่ใช่เพดาน",
          f1.load <= WIN && f1.load > LOAD_LIMIT);

    /* T2: deterministic — รอบสองได้ state เหมือนทุกไบต์ (registry + load + base) */
    CHECK("T2: รัน 2 ครั้ง → registry/load/base เหมือนกันทุกไบต์",
          field_eq(&f1, &f2) && n1 == n2);
    int log_same = 1;
    for (uint32_t i = 0; i < n1; i++)
        if (log1[i].action != log2[i].action || log1[i].file_id != log2[i].file_id ||
            log1[i].base_k != log2[i].base_k || log1[i].addr != log2[i].addr)
            log_same = 0;
    CHECK("T2b: decision trace (log) 2 รอบ เหมือนกันทุก event", log_same);

    /* T3: ทุก EXPAND สอดคล้องกับ ROI ≥ GATE; ทุก REJECT = ROI < GATE */
    {
        int expand_ok = 1, reject_ok = 1;
        uint32_t expands = 0, first_reject = 0;
        for (uint32_t i = 0; i < n1; i++) {
            const LogEv *e = &log1[i];
            if (e->action == ACT_EXPAND) {
                expands++;
                if (roi_field(e->base_k) < GATE) expand_ok = 0;
            }
            if (e->action == ACT_REJECT) {
                if (!first_reject) first_reject = i;
                if (!(roi_field(e->base_k) < GATE)) reject_ok = 0;
            }
        }
        printf("     expands=%u (k=2→3,3→4,4→5), first reject หลัง file 64\n", expands);
        CHECK("T3: EXPAND ทุกครั้ง ROI ≥ 1.0; REJECT ทุกครั้ง ROI < 1.0 (knee=5)", expand_ok && reject_ok);
        CHECK("T3b: ขยาย 3 ก้าวพอดี (2→3, 3→4, 4→5) — หยุดก่อน k=6 (ROI 0.06)", expands == 3);
        CHECK("T3c: ไม่มี entry วางที่ k≥6 — gate บล็อกสเกลที่ถ่วงกลืนกำไร", f1.field_base == 5);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T4–T6: replay — log ถูกต้อง → state เหมือนเดิมทุกไบต์
   ═══════════════════════════════════════════════════════════════ */
static void test_replay(void) {
    printf("═ REPLAY — decision trace → state เหมือนเดิมทุกไบต์ (determinism ครบ) ═\n");

    Field f1, f2;
    LogEv log1[LOG_MAX];
    uint32_t n1 = 0;
    run_scenario_a(&f1, log1, &n1);

    /* T4: replay จาก log (ไม่มี input เดิม) → byte-identical */
    field_replay(&f2, log1, n1);
    CHECK("T4: replay จาก log → registry/load/base/rejected เหมือนทุกไบต์",
          field_eq(&f1, &f2));

    /* T5: n_slots ถูก recompute จาก base_k — ปลอม event → state ต่าง (replay ไม่ใจดี) */
    {
        LogEv tampered[LOG_MAX];
        memcpy(tampered, log1, n1 * sizeof(LogEv));
        int flipped = 0;
        for (uint32_t i = 0; i < n1 && !flipped; i++)
            if (tampered[i].action == ACT_PLACE && tampered[i].base_k == 3) {
                tampered[i].base_k = 4;            /* ปลอม: อ้างว่าวางที่ k=4 */
                flipped = 1;
            }
        Field ft;
        field_replay(&ft, tampered, n1);
        CHECK("T5: ปลอม base_k ใน log → replay ให้ state ต่าง (จับได้, ไม่ใจดี)",
              flipped && !field_eq(&f1, &ft));
        CHECK("T5b: ปลอม k → load ต่าง (n_slots คำนวณจาก k — ตรวจสอบได้)",
              ft.load != f1.load);
    }

    /* T6: ตัด event ทิ้ง (log หาย) → state ต่าง — replay เท่ากับตรง ต้อง log ครบ */
    {
        Field ft;
        field_replay(&ft, log1, n1 - 1);
        CHECK("T6: log ขาด 1 event → state ต่าง (replay ไม่เติมของให้)", !field_eq(&f1, &ft));
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T7–T8: สนามนิ่ง — entry ไม่ขยับ, close/reopen ใช้เสาเข็มเดิม
   ═══════════════════════════════════════════════════════════════ */
static void test_static_field(void) {
    printf("═ STATIC FIELD — registry เป็นสนามนิ่ง: ไม่มีอะไรถูกย้าย ═\n");

    Field f;
    LogEv log[LOG_MAX];
    uint32_t n = 0;
    run_scenario_a(&f, log, &n);

    /* จำ addr ของทุก entry ก่อน close/reopen */
    uint16_t addr_before[REG_SLOTS];
    for (uint32_t i = 0; i < f.n; i++) addr_before[i] = f.reg[i].addr;
    uint32_t load_before = f.load;

    /* ปิด 10 ไฟล์ (id 1..10) → load คืน, entry ยังอยู่ */
    for (uint32_t i = 1; i <= 10; i++) field_close(&f, i, log, &n);
    CHECK("T7: ปิด 10 link → load ลด 3040 (คืนที่) แต่ entry ยังอยู่ครบ",
          f.load == load_before - 3040 && f.n == 64);

    /* เปิดกลับ 5 ไฟล์ → load คืน, addr เดิมเป๊ะ */
    for (uint32_t i = 1; i <= 5; i++) field_reopen(&f, i, log, &n);
    int addr_same = 1;
    for (uint32_t i = 0; i < f.n; i++) if (f.reg[i].addr != addr_before[i]) addr_same = 0;
    CHECK("T7b: close/reopen → addr ทุก entry ไม่เปลี่ยนเลย (เสาเข็ม — วางแล้วไม่ขยับ)",
          addr_same && f.load == load_before - 5 * 304);
    (void)addr_same;

    /* T8: addr เพิ่มขึ้นเรื่อยๆ + ลำดับ registry = ลำดับวาง (append-only, ไม่มี compaction) */
    {
        int inc = 1;
        for (uint32_t i = 1; i < f.n; i++)
            if (f.reg[i].addr <= f.reg[i - 1].addr) inc = 0;
        CHECK("T8: addr เพิ่มขึ้นเรื่อยๆ + ลำดับ = ลำดับวาง — ไม่มี reorder/compaction", inc);
    }

    /* replay เต็ม scenario (รวม close/reopen) → เหมือนทุกไบต์ */
    {
        Field fr;
        field_replay(&fr, log, n);
        CHECK("T8b: replay รวม close/reopen → state เหมือนทุกไบต์", field_eq(&f, &fr));
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T9: ปิด link คืนที่ → placement ใหม่ไม่ต้องข้ามเส้น (ไม่มี gate)
   ═══════════════════════════════════════════════════════════════ */
static void test_close_frees_capacity(void) {
    printf("═ CLOSE FREES — ไฟล์ที่เคยถูกปฏิเสธ วางได้เมื่อมีที่คืน ═\n");

    Field f;
    LogEv log[LOG_MAX];
    uint32_t n = 0;
    run_scenario_a(&f, log, &n);
    uint32_t rejected_before = f.rejected;   /* 36 */

    /* ปิด 10 ไฟล์ → load 18544-3040... ไม่: load 18892-3040 = 15852 < 18662 */
    for (uint32_t i = 1; i <= 10; i++) field_close(&f, i, log, &n);
    CHECK("T9: ปิด 10 → load < เส้น 90% (15852 < 18662)", f.load < LOAD_LIMIT);

    /* วางไฟล์ใหม่ id=200 → placement ปกติ ไม่ต้องผ่าน gate (ไม่มี EXPAND ก่อนหน้า) */
    uint32_t mark = n;
    int r = field_place(&f, 200, log, &n);
    int no_expand = 1;
    for (uint32_t i = mark; i < n; i++)
        if (log[i].action == ACT_EXPAND) no_expand = 0;
    CHECK("T9b: วางใหม่เมื่อมีที่ → placed ที่ k=5, ไม่มี EXPAND (ไม่มี gate)",
          r == R_OK && no_expand && f.reg[f.n - 1].base_k == 5);

    /* เปิดกลับ 10 ไฟล์ (คืนครบ) → load เกินเส้นอีก → gate กลับมาคุม */
    for (uint32_t i = 1; i <= 10; i++) field_reopen(&f, i, log, &n);
    CHECK("T9c: เปิดกลับครบ 10 → load เกินเส้นอีก (18968 > 18662) → gate กลับมาคุม",
          f.load > LOAD_LIMIT);
    uint32_t rj0 = f.rejected;
    int r2 = field_place(&f, 201, log, &n);
    CHECK("T9d: วางตัวใหม่ข้ามเส้น → gate ปฏิเสธอีก (ROI(5) 0.64 < 1) — determinism",
          r2 == R_REJ && f.rejected == rj0 + 1);
    CHECK("T9e: rejected_before ยังเป็น 36 — ตัวเลขทุกตัว replay ได้",
          rejected_before == 36);

    /* replay scenario นี้ครบ → เหมือนทุกไบต์ */
    {
        Field fr;
        field_replay(&fr, log, n);
        CHECK("T9f: replay ทั้ง scenario (วาง/ปฏิเสธ/ขยาย/ปิด/เปิด) → เหมือนทุกไบต์",
              field_eq(&f, &fr));
    }
    printf("\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Leverage gate x registry — วางไฟล์ข้ามเส้น 90%% → gate ขยาย/ปฏิเสธ, replay deterministic\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    test_gate_behavior();
    test_replay();
    test_static_field();
    test_close_frees_capacity();
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
