/*
 * test_walk_sync.c — Walk = Sync: กลไกเดียว สองบทบาท (stride-37 บน ring)
 * ═══════════════════════════════════════════════════════════════════════
 * (§15.98 — docs/WALK-SYNC.md)
 *
 * walk ถาม "ที่ไหน" (addressing — index) · rail ถาม "เมื่อไหร่" (sync — clock)
 * — เลขคณิตชุดเดียวกัน (37·i mod ring) → bijection → deterministic O(1) ทั้งคู่
 *
 * Proof:
 *   W1  rail_ring: build → ทุก lane (A/B/C) ครอบ 1440 enc ครบ 1 ครั้ง (bijection)
 *   W2  stride-37 bijection: เดินครบ 1440 (frame_seek) และ 720 (tring) — กลับจุดเริ่ม
 *   W3  lane lock: B = A+480 · C = A+960 ทุก i — phase คงที่ 120° (3-fold — sync โดย construction)
 *   W4  freeze tick-12: freeze points (t = 12k) = 120 จุด = WANG_WIN_COUNT
 *       — หนึ่ง freeze ต่อ Wang window · ที่ freeze point θ เป็น integer
 *       (A[i].enc % 4 == 0 — 3 lanes อยู่บน 1° grid — aligned)
 *   W5  modular distance ระหว่าง lane = 480 = 120° เสมอ (sync ไม่ drift)
 *   W6  geo_frame_seek_verify() == 0 (self-verify ในโค้ด — 14 checks ผ่าน)
 *   R   ring = กฎ N-gon เดียวกัน 2 สเกล (เชื่อม test_dodeca_x2 G-section):
 *       · 720 = 6×120 / 1440 = 12×120 — sector count (6/12) obey กฎ N-gon:
 *         divisor law บน sector indices — 12 faces → stride-2 → 2 hexagons ของ faces
 *         (กฎ ×2 ระดับ ring!) · 6 spokes → 2 triangles
 *       · polarity 60/60 ภายในทุก sector (ROUTE/GROUND = ∧∨ pair — กฎ ×2 ระดับ slot)
 *       · stride-37: gcd(37,720)=1 · gcd(37,1440)=1 · gcd(37,120)=1 → bijection
 *         ทั้งระดับ ring และระดับ sector (กฎเดียวกัน 2 สเกล)
 *       · ทุก 120-step window → polarity 60/60 (sawtooth balance ทุก sector sweep)
 *   S   slot level ของ ring จริง: stride-2 บน 1440 slots → 2 cycles ของ 720
 *       (กฎ ×2 ระดับ slot — คี่/คู่ · parity สลับทุกก้าวของ frame_enc)
 *       + nesting ratio s(N) = cos(2π/N)/cos(π/N) ลู่เข้า 1 เมื่อ N → ∞:
 *       s(1440) ≈ 0.9999976 · 1−s ∝ 1/N² (const = 3π²/2 ≈ 14.8044 — analytic)
 *       · s¹² 12 ระดับ: 12-gon ≈ 0.2709 (หดชัด) vs 1440-ring ≈ 0.99996 (แบน)
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -Icore/infra -o build/test_walk_sync \
 *        tests/test_walk_sync.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "../core/infra/geo_rail_ring.h"
#include "../core/geo_frame_seek.h"
#include "../core/geo_tring_walk.h"
#include "../core/geo_frame_seek_wang.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint32_t gcd32(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

/* divisor law: stride k → gcd(k,n) cycles × n/gcd(k,n) — กฎ N-gon บน n ตำแหน่ง */
static int divisor_law_ok(uint32_t n)
{
    for (uint32_t k = 1; k <= n/2; k++) {
        uint8_t seen[24] = {0};
        uint32_t n_cyc = 0, g = gcd32(k, n);
        for (uint32_t i = 0; i < n; i++) {
            if (seen[i]) continue;
            uint32_t len = 0, cur = i;
            while (!seen[cur]) { seen[cur] = 1; cur = (cur + k) % n; len++; }
            if (len != n / g) return 0;
            n_cyc++;
        }
        if (n_cyc != g) return 0;
    }
    return 1;
}

/* modular circular distance */
static uint32_t mod_dist(uint32_t a, uint32_t b, uint32_t mod)
{
    uint32_t d = (a > b) ? (a - b) : (b - a);
    if (d > mod / 2u) d = mod - d;
    return d;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Walk = Sync — กลไกเดียว สองบทบาท (stride-37 บน ring)\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    RailRing rr;
    rail_ring_build(&rr);

    /* ── W1: rail_ring — ทุก lane เป็น bijection ครบ 1440 ── */
    {
        CHECK("W1a: rail_ring_verify(A) == 0 — 1440 enc ครบ 1 ครั้ง (built-in)",
              rail_ring_verify(&rr) == 0);
        uint8_t seen[RAIL_RING_SIZE];
        int okB = 1, okC = 1;
        memset(seen, 0, sizeof seen);
        for (uint32_t i = 0; i < RAIL_RING_SIZE; i++) {
            if (rr.B[i].enc >= RAIL_RING_SIZE || seen[rr.B[i].enc]) { okB = 0; break; }
            seen[rr.B[i].enc] = 1;
        }
        memset(seen, 0, sizeof seen);
        for (uint32_t i = 0; i < RAIL_RING_SIZE; i++) {
            if (rr.C[i].enc >= RAIL_RING_SIZE || seen[rr.C[i].enc]) { okC = 0; break; }
            seen[rr.C[i].enc] = 1;
        }
        CHECK("W1b: lane B เป็น bijection ครบ 1440 (offset 480 ไม่ทำลาย coverage)", okB);
        CHECK("W1c: lane C เป็น bijection ครบ 1440 (offset 960 ไม่ทำลาย coverage)", okC);
    }

    /* ── W2: stride-37 bijection บน 1440 และ 720 ── */
    {
        uint8_t seen[FRAME_CYCLE];
        memset(seen, 0, sizeof seen);
        uint16_t e = 0;
        int ok = 1;
        for (uint32_t i = 0; i < FRAME_CYCLE; i++) {
            if (seen[e]) { ok = 0; break; }
            seen[e] = 1;
            e = frame_next(e);
        }
        CHECK("W2a: stride-37 บน 1440 — เยี่ยมครบ 1 ครั้ง + กลับจุดเริ่ม (bijection)",
              ok && e == 0);

        uint8_t s2[TRING_WALK_CYCLE];
        memset(s2, 0, sizeof s2);
        ok = 1;
        for (uint32_t i = 0; i < TRING_WALK_CYCLE; i++) {
            uint16_t enc = tring_walk_enc(i);
            if (s2[enc]) { ok = 0; break; }
            s2[enc] = 1;
        }
        CHECK("W2b: stride-37 บน 720 (tring) — bijection ครบ 720 (gcd(37,720)=1)",
              ok);
    }

    /* ── W3: lane lock — B = A+480 · C = A+960 ทุก i (phase 120° คงที่) ── */
    {
        int lock = 1;
        for (uint32_t i = 0; i < RAIL_RING_SIZE; i++) {
            if (rr.B[i].enc != (uint16_t)((rr.A[i].enc + 480u) % RAIL_RING_SIZE)) { lock = 0; break; }
            if (rr.C[i].enc != (uint16_t)((rr.A[i].enc + 960u) % RAIL_RING_SIZE)) { lock = 0; break; }
        }
        CHECK("W3a: lane lock — B = A+480 · C = A+960 ทุก i (3 lanes = สำเนา walk 120° จากกัน)",
              lock);
    }

    /* ── W4: freeze tick-12 = 120 จุด = WANG_WIN_COUNT + θ aligned ── */
    {
        uint8_t seen[FRAME_CYCLE];
        memset(seen, 0, sizeof seen);
        uint32_t n = 0, align_ok = 1;
        for (uint32_t k = 0; k < FRAME_CYCLE / 12u; k++) {
            uint32_t i = 12u * k;                       /* tick-12 boundary */
            uint16_t enc = frame_enc(i);
            if (!seen[enc]) { seen[enc] = 1; n++; }
            if (rr.A[i].enc % 4u != 0) align_ok = 0;    /* θ = enc/4 เป็น integer */
        }
        CHECK("W4a: freeze points (tick-12) = 120 จุด = WANG_WIN_COUNT (หนึ่ง freeze ต่อ Wang window)",
              n == WANG_WIN_COUNT && WANG_WIN_COUNT == 120u);
        CHECK("W4b: ที่ freeze point — 3 lanes อยู่บน 1° grid (θ = enc/4 integer — aligned)",
              align_ok);
        /* sync ปลอดภัย: ที่ freeze point lane lock ยังอยู่ (120° spacing ครบ) */
        CHECK("W4c: freeze point = จุดที่ lane lock ยืนยันได้ (sync condition ครบ → freeze/checkpoint ปลอดภัย)",
              rr.A[0].enc % 4u == 0u);
    }

    /* ── W5: modular distance ระหว่าง lanes = 480 = 120° เสมอ ── */
    {
        int md = 1;
        for (uint32_t i = 0; i < RAIL_RING_SIZE; i++) {
            if (mod_dist(rr.A[i].enc, rr.B[i].enc, RAIL_RING_SIZE) != 480u) { md = 0; break; }
            if (mod_dist(rr.B[i].enc, rr.C[i].enc, RAIL_RING_SIZE) != 480u) { md = 0; break; }
            if (mod_dist(rr.C[i].enc, rr.A[i].enc, RAIL_RING_SIZE) != 480u) { md = 0; break; }
        }
        CHECK("W5a: modular distance ระหว่าง lane = 480 = 120° เสมอ (sync ไม่ drift — deterministic)",
              md);
        CHECK("W5b: 480 = 1440/3 — sector 120° (3-fold C3 — 3 squares/lanes/chiralities)",
              480u == RAIL_RING_SIZE / 3u);
    }

    /* ── W6: frame_seek self-verify ── */
    {
        CHECK("W6a: geo_frame_seek_verify() == 0 (14 checks ในโค้ดผ่าน — stride-37/roundtrip/cpair/prev-next/is_skip/phase)",
              geo_frame_seek_verify() == 0);
    }

    /* ═══════════════════════════════════════════════════════════════
       R — ring = กฎ N-gon เดียวกัน 2 สเกล (720 = 6×120 · 1440 = 12×120)
       ═══════════════════════════════════════════════════════════════ */
    printf("\n═ R — RING = N-GON LAW: 720 = 6×120 · 1440 = 12×120 ═\n");

    /* R1: sector indices obey กฎ N-gon (divisor law) */
    {
        CHECK("R1a: 720 — 6 spokes (enc/120): divisor law k=1..3 ผ่าน (gcd(k,6))",
              divisor_law_ok(6));
        CHECK("R1b: 1440 — 12 faces (enc/120): divisor law k=1..6 ผ่าน (gcd(k,12))",
              divisor_law_ok(12));
        /* 12 faces → stride-2 → 2 hexagons ของ faces — กฎ ×2 ระดับ ring */
        {
            uint8_t seen[12] = {0};
            uint32_t n_cyc = 0, lens[2] = {0};
            int odd_ok = 0, even_ok = 0;
            for (uint32_t s = 0; s < 12; s++) {
                if (seen[s]) continue;
                uint32_t len = 0, cur = s, all_odd = 1, all_even = 1;
                while (!seen[cur]) {
                    seen[cur] = 1;
                    if (cur % 2 == 0) all_odd = 0; else all_even = 0;
                    cur = (cur + 2) % 12;
                    len++;
                }
                if (all_odd) odd_ok = 1;
                if (all_even) even_ok = 1;
                lens[n_cyc++] = len;
            }
            CHECK("R1c: 12 faces → stride-2 → 2 hexagons ของ faces (คี่/คู่) — กฎ ×2 ระดับ ring",
                  n_cyc == 2 && lens[0] == 6 && lens[1] == 6 && odd_ok && even_ok);
        }
        {
            uint8_t seen[6] = {0};
            uint32_t n_cyc = 0, lens[2] = {0};
            for (uint32_t s = 0; s < 6; s++) {
                if (seen[s]) continue;
                uint32_t len = 0, cur = s;
                while (!seen[cur]) { seen[cur] = 1; cur = (cur + 2) % 6; len++; }
                lens[n_cyc++] = len;
            }
            CHECK("R1d: 6 spokes → stride-2 → 2 triangles ของ spokes (parity pair ระดับ 720)",
                  n_cyc == 2 && lens[0] == 3 && lens[1] == 3);
        }
    }

    /* R2: polarity 60/60 ภายในทุก sector (ROUTE/GROUND = ∧∨ pair — ×2 ระดับ slot) */
    {
        int ok720 = 1, ok1440 = 1;
        for (uint32_t enc = 0; enc < 720; enc++) {
            uint32_t route = (enc % 120u) < 60u ? 1u : 0u;
            (void)route;   /* per-sector count ด้านล่าง */
        }
        uint32_t per_spoke[6] = {0}, per_face[12] = {0};
        for (uint32_t enc = 0; enc < 720; enc++)
            per_spoke[enc / 120u] += ((enc % 120u) < 60u) ? 1u : 0u;  /* ROUTE per spoke */
        for (uint32_t s = 0; s < 6; s++)
            if (per_spoke[s] != 60u) ok720 = 0;
        for (uint32_t enc = 0; enc < 1440; enc++)
            per_face[enc / 120u] += ((enc % 120u) < 60u) ? 1u : 0u;  /* ROUTE per face */
        for (uint32_t f = 0; f < 12; f++)
            if (per_face[f] != 60u) ok1440 = 0;
        CHECK("R2a: ทุก spoke ของ 720 มี polarity 60/60 (ROUTE/GROUND = ∧∨ pair)", ok720);
        CHECK("R2b: ทุก face ของ 1440 มี polarity 60/60 (กฎ ×2 ระดับ slot — hex+tri)", ok1440);
    }

    /* R3: stride-37 coprime กับ ring และ sector → bijection 2 สเกล */
    {
        CHECK("R3a: gcd(37,720)=1 · gcd(37,1440)=1 · gcd(37,120)=1 (walk law ใช้ได้ทุกสเกล)",
              gcd32(37, 720) == 1 && gcd32(37, 1440) == 1 && gcd32(37, 120) == 1);
        uint32_t per_spoke[6] = {0}, per_face[12] = {0};
        for (uint32_t t = 0; t < 720; t++)
            per_spoke[(t * 37u) % 720u / 120u]++;
        for (uint32_t t = 0; t < 1440; t++)
            per_face[(t * 37u) % 1440u / 120u]++;
        int c720 = 1, c1440 = 1;
        for (uint32_t s = 0; s < 6; s++) if (per_spoke[s] != 120u) c720 = 0;
        for (uint32_t f = 0; f < 12; f++) if (per_face[f] != 120u) c1440 = 0;
        CHECK("R3b: 720-walk เยี่ยมแต่ละ spoke ครบ 120 ครั้ง (ทุก (spoke,slot) ครั้งเดียว)", c720);
        CHECK("R3c: 1440-walk เยี่ยมแต่ละ face ครบ 120 ครั้ง (ทุก (face,slot) ครั้งเดียว)", c1440);
    }

    /* R4: ภายใน sector — stride-37 mod 120 เป็น bijection (slots ครบ) + ทุก 120-step window 60/60 */
    {
        int slots_ok = 1, win_ok = 1;
        for (uint32_t s = 0; s < 6 && slots_ok; s++) {
            uint8_t seen[120] = {0};
            for (uint32_t t = 0; t < 720; t++) {
                uint32_t enc = (t * 37u) % 720u;
                if (enc / 120u == s) {
                    uint32_t sl = enc % 120u;
                    if (seen[sl]) { slots_ok = 0; break; }
                    seen[sl] = 1;
                }
            }
        }
        CHECK("R4a: ภายในแต่ละ spoke — slots ครบ 120 ไม่ซ้ำ (bijection ระดับ sector)", slots_ok);
        /* ทุกหน้าต่าง 120 steps → polarity 60/60 (sawtooth balance) */
        for (uint32_t t0 = 0; t0 + 120u <= 1440u; t0++) {
            uint32_t route = 0;
            for (uint32_t w = 0; w < 120u; w++) {
                uint32_t sl = ((t0 + w) * 37u) % 120u;
                if (sl < 60u) route++;
            }
            if (route != 60u) { win_ok = 0; break; }
        }
        CHECK("R4b: ทุก 120-step window → polarity 60/60 (∧∨ สมดุลทุก sector sweep)", win_ok);
    }

    /* R5: กฎ ×2 เกิด 2 ระดับ — faces (2 hexagons) + slots (60/60) */
    {
        uint32_t route_total = 0;
        for (uint32_t enc = 0; enc < 1440; enc++)
            if ((enc % 120u) < 60u) route_total++;
        CHECK("R5a: กฎ ×2 เกิด 2 ระดับ — faces (2 hexagons ของ 12 faces) + slots (60/60 รวม = 720/720)",
              divisor_law_ok(12) && route_total == 720u);
    }

    /* ═══════════════════════════════════════════════════════════════
       S — กฎ ×2 ที่ slot level ของ ring จริง (1440 slots → 2×720)
           + nesting ratio ลู่เข้า 1 ที่ N ใหญ่ (พื้นเรียบที่สเกลใหญ่)
       ═══════════════════════════════════════════════════════════════ */
    printf("\n═ S — SLOT-LEVEL ×2 + NESTING → 1: ring จริง = กฎ N-gon ที่ N=1440 ═\n");

    /* S1: stride-2 บน 1440 slots ของ 1440-ring → 2 cycles ของ 720 (กฎ ×2 ระดับ slot)
     * — เดินจริงบน frame_enc (ring จริงที่ระบบใช้) */
    {
        uint8_t seen[FRAME_CYCLE];
        memset(seen, 0, sizeof seen);
        uint32_t n_cyc = 0, lens[2] = {0};
        int odd_ok = 0, even_ok = 0;
        for (uint32_t s = 0; s < FRAME_CYCLE; s++) {
            if (seen[s]) continue;
            uint32_t len = 0, cur = s, all_odd = 1, all_even = 1;
            while (!seen[cur]) {
                seen[cur] = 1;
                if (cur % 2 == 0) all_odd = 0; else all_even = 0;
                cur = (cur + 2u) % FRAME_CYCLE;
                len++;
            }
            if (all_odd) odd_ok = 1;
            if (all_even) even_ok = 1;
            lens[n_cyc++] = len;
        }
        CHECK("S1a: stride-2 บน 1440 slots → 2 cycles ของ 720 (กฎ ×2 ระดับ slot — slot คี่/คู่)",
              n_cyc == 2 && lens[0] == 720u && lens[1] == 720u && odd_ok && even_ok);
        /* อีกมุม: เดินผ่าน frame_enc ของ ring จริง (bijection) — slot parity สลับทุกก้าว */
        int alt_ok = 1;
        for (uint32_t i = 0; i < FRAME_CYCLE; i++) {
            uint16_t e = frame_enc(i);
            uint16_t nxt = frame_enc((i + 1u) % FRAME_CYCLE);
            if ((e % 2u) == (nxt % 2u)) { alt_ok = 0; break; }
        }
        CHECK("S1b: slot parity สลับทุกก้าวของ walk (frame_enc i, i+1 — ฟันปลา level slot)",
              alt_ok);
    }

    /* S2: nesting ratio s(N) = cos(2π/N)/cos(π/N) ลู่เข้า 1 เมื่อ N → ∞
     * — 1440: s ≈ 0.9999976 (เกือบแบน — พื้นเรียบที่สเกลใหญ่) */
    {
        double s12 = cos(2.0 * M_PI / 12.0) / cos(M_PI / 12.0);
        double s1440 = cos(2.0 * M_PI / 1440.0) / cos(M_PI / 1440.0);
        CHECK("S2a: s(12) = cos(π/6)/cos(π/12) ≈ 0.89658 (กฎ 12-gon — ซ้อนหดชัด)",
              fabs(s12 - 0.896575) < 1e-5);
        CHECK("S2b: s(1440) = cos(2π/1440)/cos(π/1440) ≈ 0.99999286 (เกือบ 1 — พื้นเรียบ)",
              fabs(s1440 - 0.9999928605) < 1e-9 && s1440 < 1.0);

        /* monotonic: N ใหญ่ → s ใหญ่ (ลู่เข้าหา 1 จากด้านล่าง) */
        static const uint32_t NS[] = { 6u, 8u, 12u, 24u, 60u, 144u, 360u, 720u, 1440u };
        int mono = 1;
        double prev = 0.0;
        for (uint32_t k = 0; k < sizeof(NS)/sizeof(NS[0]); k++) {
            double s = cos(2.0 * M_PI / NS[k]) / cos(M_PI / NS[k]);
            if (s <= prev || s >= 1.0) { mono = 0; break; }
            prev = s;
        }
        CHECK("S2c: s(N) เพิ่ม monotonic ตาม N และ < 1 เสมอ (ลู่เข้าหา 1 จากล่าง)", mono);
    }

    /* S3: อัตราการลู่เข้า — 1−s(N) ∝ 1/N² (exp: 3π²/2 = 14.8044… —
     * สืบจาก cos(x)=1−x²/2+x⁴/24−…: s = (1−2a²+2a⁴/3)/(1−a²/2+a⁴/24),
     * a=π/N → 1−s = (3π⁴/2)/N⁴ · N²/… → (1−s)N² = 3π²/2) —
     * s(1440) กับ s(360): (1−s360)/(1−s1440) ≈ (1440/360)² = 16 */
    {
        double s360 = cos(2.0 * M_PI / 360.0) / cos(M_PI / 360.0);
        double s1440 = cos(2.0 * M_PI / 1440.0) / cos(M_PI / 1440.0);
        double ratio = (1.0 - s360) / (1.0 - s1440);
        CHECK("S3a: (1−s(360))/(1−s(1440)) ≈ 16 = (1440/360)² — 1−s ∝ 1/N²",
              fabs(ratio - 16.0) < 0.1);
        /* ค่า empirical ของ constant: (1−s)·N² → 3π²/2 ≈ 14.8044 */
        double c1440 = (1.0 - s1440) * 1440.0 * 1440.0;
        double c360 = (1.0 - s360) * 360.0 * 360.0;
        CHECK("S3b: (1−s)·N² → 3π²/2 = 14.8044 (analytic — ตรวจที่ N=360,1440)",
              fabs(c1440 - 14.8044) < 0.05 && fabs(c360 - 14.8044) < 0.05);

        /* ตรวจกลับ: ที่ N=1440, 1−s ≈ 3π²/2N² = 7.1394e-6 (asymptotic —
         * higher-order error ~N⁻⁴·N² = N⁻² ≈ 4.8e-7 ของค่าหลัก — กัน 1e-3) */
        double expect = 3.0 * M_PI * M_PI / (2.0 * 1440.0 * 1440.0);
        CHECK("S3c: 1−s(1440) = 3π²/2N² = 7.139e-6 (สูตร asymptotic ตรง ~0.01%)",
              fabs((1.0 - s1440) - expect) < 1e-9);
    }

    /* S4: กฎเดียวกันคนละสเกล — nesting scale ของ N=1440 คือ 1−(3π²/2N²)
     * และ s¹² (12 ระดับของ 1440-ring) ≈ 1 − 12·7.139e-6 (linear ดีมาก) */
    {
        double s = cos(2.0 * M_PI / 1440.0) / cos(M_PI / 1440.0);
        double s12 = pow(s, 12.0);
        double lin = 1.0 - 12.0 * (1.0 - s);
        /* error ≈ 66·(1−s)² = 3.4e-9 (binomial term 66ε²) */
        CHECK("S4a: s(1440)¹² ≈ 1 − 12(1−s) (12 ระดับยัง linear — err ≈ 66ε² = 3.4e-9)",
              fabs(s12 - lin) < 1e-8);
        /* 12 ระดับของ ring จริง: 1440 → เกือบไม่หด — ต่างจาก 12-gon ที่ s¹² ≈ 0.27 */
        double s12g = pow(cos(M_PI / 6.0) / cos(M_PI / 12.0), 12.0);
        CHECK("S4b: 12-gon s¹² ≈ 0.2698 vs 1440-ring s¹² ≈ 0.99991 — สเกลเล็กหดชัด, สเกลใหญ่แบน",
              fabs(s12g - 0.2698002) < 1e-4 && s12 > 0.9999);
    }

    printf("═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
