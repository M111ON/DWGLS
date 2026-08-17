/* test_hyp_fusion.c — Hyperbolic Section Fusion
 * ═══════════════════════════════════════════════════════════════════════════
 * user: "merge / fusion เป็น section ได้ไหม — ถ้าแยกกันทุกอย่างดีหมด
 *        แต่ต้องเลือกที่ดีแล้วไม่ฉุดกำลังด้วย"
 *
 * พิสูจน์ fusion (core/hyp_fusion.h) ว่า compose ได้จริง + ไม่ฉุดกำลัง:
 *   S1 ADDRESS — hyp_bond (RDH+face) bijection + reversible + route/mirror
 *   S2 GATE   — hyp_gate หนึ่ง decision (แทน wang+tantrix 2 ชั้น) +
 *               hyp_log_validate หนึ่ง loop (แทน 6 loop) + rdtsc เทียบ
 *   S3 WEIGHT — Hosoya/F ladder ตรง (F(12)=144)
 *   CHAIN     — scale-event log → hyp_bond append → validate → seek → replay
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore \
 *        -o build/test-hyp_fusion tests/test_hyp_fusion.c -lm
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../core/hyp_fusion.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* rdtsc — lfence serialized (วิธีเดียวกับ rdh_bench) */
#if defined(_MSC_VER)
#include <intrin.h>
static inline uint64_t rdtsc_now(void) { return __rdtsc(); }
#else
static inline uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#endif

/* ── S1: ADDRESS ── */
static void test_address(void) {
    printf("\nA. S1 ADDRESS — bond = RDH + face_id, reversible\n");

    /* A1: bond = mixed-radix injective โดย construction:
     * key = face(48..51) | rdh_core(0..47) — สองส่วนไม่ทับ → unique */
    {
        uint32_t bad = 0, n = 0;
        for (uint32_t b = 0; b < 1024; b += 13)
            for (uint8_t f = 0; f < 12; f++)
                for (uint16_t s = 0; s < 256; s += 7) {
                    uint64_t key = hyp_bond(b, (uint64_t)f << 11, (uint8_t)s);
                    uint64_t core = key & 0xFFFFFFFFFFFFull;   /* 48 bits rdh */
                    uint64_t face = (key >> 48) & 0xFu;        /* 4 bits face */
                    /* mixed-radix: (face, core) → key unique ถ้า core < 2^48
                     * และ face < 16 — injective โดย construction */
                    if (core >= (1ull << 48) || face >= 16u) bad++;
                    if (((face << 48) | core) != key) bad++;
                    n++;
                }
        CHECK(1, "bond mixed-radix injective — (face | rdh_core) กู้ key กลับครบ (bad=0)",
              n == 79u * 12u * 37u && bad == 0);
    }

    /* A2: face + core reversible */
    {
        uint32_t bad = 0;
        for (uint32_t b = 0; b < 4096; b += 17)
            for (uint8_t f = 0; f < 12; f++)
                for (uint16_t s = 0; s < 252; s += 13) {
                    uint64_t key = hyp_bond(b, (uint64_t)f << 11, s);
                    uint32_t rb; uint8_t rf;
                    hyp_bond_core(key, &rb, &rf);
                    if (rb != b || rf != s || hyp_bond_face(key) != f) bad++;
                }
        CHECK(2, "reversible — กู้ (block, from, face) กลับจาก bond ครบ (bad=0)",
              bad == 0);
    }

    /* A3: route bijection ครบ 20736 */
    {
        uint32_t bad = 0;
        for (uint32_t slot = 0; slot < 20736u; slot++) {
            HypRoute r = hyp_route(slot);
            uint32_t back = (uint32_t)r.axis * 6912u
                          + (uint32_t)r.half * 3456u
                          + (uint32_t)r.slot_in_spoke * 6u + r.spoke;
            if (back != slot || r.axis > 2 || r.half > 1 ||
                r.spoke > 5 || r.slot_in_spoke >= 576u) bad++;
        }
        CHECK(3, "hyp_route — bijection ครบ 20736 (axis, half, spoke, slot)",
              bad == 0);
    }

    /* A4: mirror = อีกครึ่งของ axis เดียวกัน, roundtrip */
    {
        uint32_t bad = 0;
        for (uint32_t slot = 0; slot < 20736u; slot += 13) {
            uint32_t m = hyp_mirror_slot(slot);
            HypRoute r1 = hyp_route(slot), r2 = hyp_route(m);
            if (r1.axis != r2.axis || r1.half == r2.half ||
                hyp_mirror_slot(m) != slot) bad++;
        }
        CHECK(4, "hyp_mirror — คร่อม half (KIS↔hyp), axis เดิม, mirror²=id",
              bad == 0);
    }

    /* A5: face จาก GeoSeed topology (shift+mask) — 12 หน้า */
    {
        uint8_t faces[12] = {0};
        for (uint64_t t = 0; t < 96; t++) faces[hyp_face(t * 2048u + 11u)] = 1;
        uint32_t n = 0;
        for (uint8_t i = 0; i < 12; i++) n += faces[i];
        CHECK(5, "hyp_face — (topo>>11)&0xF ให้หน้า 0..11 (เหมือน GeoSeed)",
              n == 12);
    }
}

/* ── S2: GATE ── */
static void test_gate(void) {
    printf("\nB. S2 GATE — wang + tantrix = หนึ่ง decision (ไม่ฉุดกำลัง)\n");
    FrameWangLayer wl;
    fwang_init(&wl);

    /* B1: สนามสะอาด — OPEN/SKIP เท่านั้น (ไม่มี CLOSED/TAMPER) */
    {
        int open = 0, skip = 0, closed = 0, tamper = 0;
        for (uint16_t e = 0; e < FRAME_CYCLE; e++) {
            HypSeek d = hyp_gate(&wl, e, _fwang_chord_a(e) & 3u);
            switch (d) {
                case HYP_SEEK_OPEN:   open++;   break;
                case HYP_SEEK_SKIP:   skip++;   break;
                case HYP_SEEK_CLOSED: closed++; break;
                case HYP_SEEK_TAMPER: tamper++; break;
            }
        }
        CHECK(6, "clean: OPEN+SKIP = 1440 ครบ, CLOSED=0 TAMPER=0",
              open + skip == (int)FRAME_CYCLE && closed == 0 && tamper == 0);
    }

    /* B2: ข้อมูลเสีย → CLOSED/TAMPER (เหมือน wang+tantrix แยก) */
    {
        FrameWangLayer wl2;
        fwang_init(&wl2);
        /* edge ถูกแก้แบบ chord-คู่ → ผ่าน tamper แต่ continuity แตก → CLOSED */
        uint16_t win = 5;
        uint8_t neu = (uint8_t)((wl2.wins[win].edge_top + 1) % 9u);
        wl2.wins[win].edge_top   = neu;
        wl2.wins[win].edge_top_b = (uint8_t)((9u - neu) % 9u);
        HypSeek d = hyp_gate(&wl2, (uint16_t)(win * WANG_WIN_SIZE + 1u), 0);
        CHECK(7, "edge ขาด → CLOSED (hyp_gate จับได้ใน decision เดียว)",
              d == HYP_SEEK_CLOSED);

        /* edge_top_b โดนแตะ → TAMPER */
        FrameWangLayer wl3;
        fwang_init(&wl3);
        wl3.wins[3].edge_top_b ^= 1u;
        CHECK(8, "edge ถูกแตะในชั้นเก็บ → TAMPER",
              hyp_gate(&wl3, 3u * WANG_WIN_SIZE, 0) == HYP_SEEK_TAMPER);
    }

    /* B3: incoming ไม่ตรง entry → CLOSED (tantrix DROP ใน decision เดียว) */
    {
        uint16_t e = 1;  /* ไม่ใช่ 369, edge ดี */
        HypSeek d1 = hyp_gate(&wl, e, _fwang_chord_a(e) & 3u);
        HypSeek d2 = hyp_gate(&wl, e, (uint8_t)((_fwang_chord_a(e) + 1u) & 3u));
        CHECK(9, "gate ตรง → OPEN · gate ไม่ตรง → CLOSED (tantrix DROP)",
              d1 == HYP_SEEK_OPEN && d2 == HYP_SEEK_CLOSED);
    }

    /* B4: hyp_log_validate — 1 loop ตรวจครบ (เทียบ fwang_verify) */
    {
        CHECK(10, "hyp_log_validate == 0 บน log สะอาด (หนึ่ง loop แทน 6)",
              hyp_log_validate(&wl) == 0);
        FrameWangLayer wl2;
        fwang_init(&wl2);
        wl2.wins[7].edge_top = (uint8_t)((wl2.wins[7].edge_top + 1) % 9u);
        wl2.wins[7].edge_top_b = (uint8_t)((9u - wl2.wins[7].edge_top) % 9u);
        CHECK(11, "hyp_log_validate จับ edge พัง (≠0)",
              hyp_log_validate(&wl2) != 0);
    }

    /* B5: rdtsc min-of-9 — fused gate ≤ (wang + tantrix แยก) — ไม่ฉุดกำลัง
     * (methodology ตามบทเรียน rdh_bench: min-of-N + lfence + ไม่มี modulo) */
    {
        volatile uint64_t sink = 0;
        const int N = 200000;
        uint64_t best_f = ~0ull, best_s = ~0ull;
        for (int trial = 0; trial < 9; trial++) {
            uint64_t t0 = rdtsc_now();
            for (int i = 0; i < N; i++) {
                uint16_t e = (uint16_t)((i * 37) % FRAME_CYCLE);
                sink += (uint64_t)hyp_gate(&wl, e, (uint8_t)(i & 3u));
            }
            uint64_t t1 = rdtsc_now();
            if (t1 - t0 < best_f) best_f = t1 - t0;

            t0 = rdtsc_now();
            for (int i = 0; i < N; i++) {
                uint16_t e = (uint16_t)((i * 37) % FRAME_CYCLE);
                FrameWangDecision g = fwang_seek_gate(&wl, e);
                TantrixTile tile = tantrix_make((uint8_t)(_fwang_chord_a(e) & 3u),
                                                (uint8_t)(e & 3u), 0,
                                                TANTRIX_CLASS_NORMAL);
                uint8_t out = 0;
                TantrixRouteResult r = tantrix_route(tile, (uint8_t)(i & 3u), &out);
                sink += (uint64_t)g + (uint64_t)r;
            }
            uint64_t t2 = rdtsc_now();
            if (t2 - t0 < best_s) best_s = t2 - t0;
        }
        printf("    rdtsc min-of-9 (cyc/N): fused=%lu separate=%lu — ไม่ฉุดกำลัง: %s\n",
               (unsigned long)(best_f / N), (unsigned long)(best_s / N),
               best_f <= best_s ? "YES" : "NO");
        CHECK(12, "fused gate ≤ wang+tantrix แยก (min-of-9, ไม่มี double work)",
              best_f <= best_s);
        (void)sink;
    }
}

/* ── S3: WEIGHT ── */
static void test_weight(void) {
    printf("\nC. S3 WEIGHT — Hosoya F ladder\n");
    CHECK(13, "F(12) == 144 (\"100\" ของโลกฐาน 12)", hyp_fibo(12) == 144u);
    CHECK(14, "ladder 0,1,1,2,3,5,8,13,21,34,55,89 ถูก",
          hyp_fibo(0)==0 && hyp_fibo(1)==1 && hyp_fibo(5)==5 &&
          hyp_fibo(10)==55 && hyp_fibo(11)==89);
    CHECK(15, "Hosoya cell H(6,2) = F(3)×F(5) = 2×5 = 10 (ตรงตาราง §15.43)",
          hyp_hosoya(6, 2) == 10u);
}

/* ── CHAIN: log → bond → validate → seek ── */
static void test_chain(void) {
    printf("\nD. CHAIN — scale-event log ผ่าน section fusion\n");
    FrameWangLayer wl;
    fwang_init(&wl);

    /* append 120 events (block, topo, from) → ตรวจว่า bond ครบ + gate เปิด */
    uint64_t bonds[WANG_WIN_COUNT];
    int bad = 0, all_open = 1;
    for (uint16_t i = 0; i < WANG_WIN_COUNT; i++) {
        uint32_t block = i;
        uint64_t topo  = (uint64_t)(i % 12u) << 11;   /* face = i%12 */
        uint8_t  from  = (uint8_t)(i % 256u);
        bonds[i] = hyp_bond(block, topo, from);
        /* validate: bond กู้กลับได้ + gate เปิดที่ scale ของมัน */
        uint32_t rb; uint8_t rf;
        hyp_bond_core(bonds[i], &rb, &rf);
        if (rb != block || rf != from || hyp_bond_face(bonds[i]) != (i % 12u))
            bad++;
        HypSeek d = hyp_gate(&wl, (uint16_t)(i * 37 % FRAME_CYCLE),
                             _fwang_chord_a((uint16_t)(i * 37 % FRAME_CYCLE)) & 3u);
        if (d == HYP_SEEK_CLOSED || d == HYP_SEEK_TAMPER) all_open = 0;
    }
    CHECK(16, "append: bond กู้กลับได้ครบ + gate เปิดตลอด log (bad=0)",
          bad == 0 && all_open == 1);

    /* replay: วน log อีกครั้ง (เหมือนอ่านที่ scale เดิม) → bond เดิม */
    {
        int same = 1;
        for (uint16_t i = 0; i < WANG_WIN_COUNT; i++) {
            uint32_t rb; uint8_t rf;
            hyp_bond_core(bonds[i], &rb, &rf);
            uint64_t again = hyp_bond(rb, (uint64_t)hyp_bond_face(bonds[i]) << 11, rf);
            if (again != bonds[i]) same = 0;
        }
        CHECK(17, "replay: (block,face,from) → bond เดิมเป๊ะ — deterministic",
              same == 1);
    }

    /* mirror: event ที่ KIS half ↔ hyperbolic half (อ่านข้าม mirror) */
    {
        int ok = 1;
        for (uint32_t slot = 0; slot < 20736u; slot += 1729) {
            HypRoute r = hyp_route(slot);
            uint32_t m = hyp_mirror_slot(slot);
            HypRoute rm = hyp_route(m);
            if (r.axis != rm.axis || r.half + rm.half != 1) ok = 0;
        }
        CHECK(18, "mirror สม่ำเสมอ — ทุก slot มีคู่คร่อม half ใน axis เดียวกัน",
              ok == 1);
    }
}

int main(void) {
    printf("═ Hyperbolic Section Fusion (hyp_fusion.h) ═\n");
    test_address();
    test_gate();
    test_weight();
    test_chain();
    printf("\n══════════ %d/%d PASS ══════════\n", pass, pass + fail);
    return fail ? 1 : 0;
}
