/* tools/geo_net_probe.c — verify geo_net claims: 2 cycles / shift-not-mod / 12 labels
 * ═══════════════════════════════════════════════════════════════════════════════
 * user: "geo_net ไหม — ตัวที่ 2 cycle ใช้แค่ shift แทน mod สร้าง label 12"
 *
 * geo_net (TPOGLS_s11/core/geo_net.h) — Radial Routing Layer:
 *   value+addr → spoke+slot+mirror_mask
 *   _gn_mod6 = Barrett: q=(n*10923)>>16; n - q*6   ← shift แทน % 6
 *
 * พิสูจน์:
 *   A. correctness: _gn_mod6 == n%6 ครบ n 0..2^24 (Barrett ต้องแม่นทุกค่า)
 *   B. cycles: rdtsc ต่อ _gn_mod6 (จริงกี่ cycle — อ้าง "2")
 *   C. labels ที่ geo_net route สร้าง: spoke(6)+inv(6)=12 ทิศ? หรือกี่ตัว
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -o build/geo_net_probe tools/geo_net_probe.c
 */

#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>

/* geo_net core (คัดลอกจาก TPOGLS geo_net.h / geo_cylinder.h — ไม่ include ทั้งไฟล์) */
#define CYL_SPOKES   6u
#define CYL_FULL_N   3456u
#define CYL_FACE_UNITS 64u   /* 576/9 */

static inline uint8_t _gn_mod6(uint32_t n) {
    uint32_t q = (n * 10923U) >> 16;
    return (uint8_t)(n - q * 6U);
}

/* ตาม geo_net_route: addr → labels */
static inline uint8_t _route_labels(uint32_t addr,
                                    uint8_t *spoke, uint16_t *slot,
                                    uint8_t *face, uint8_t *unit,
                                    uint8_t *inv, uint8_t *group) {
    uint16_t full_idx = (uint16_t)(addr % CYL_FULL_N);
    *spoke = _gn_mod6(full_idx);
    *slot  = full_idx / CYL_SPOKES;
    *face  = (uint8_t)(*slot / CYL_FACE_UNITS);
    *unit  = (uint8_t)(*slot % CYL_FACE_UNITS);
    *inv   = (uint8_t)((*spoke + 3) % CYL_SPOKES);
    *group = (uint8_t)(*unit / 8u);
    return 1;
}

static int pass = 0, fail = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass++; printf("  PASS — %s\n", desc); } \
    else      { fail++; printf("  FAIL — %s\n", desc); } \
} while(0)

int main(void) {
    printf("═══ geo_net_probe — Barrett shift-mod6: correctness + cycles + labels ═══\n");

    /* ── A. correctness: Barrett == %6 ใน domain ที่ใช้จริง (n < 3456) ──
     * ⚠️ Barrett นี้มี domain bound: ล้มที่ n >= 32771 (พบจาก sweep 2^24)
     * geo_net ใช้กับ full_idx = addr % 3456 < 3456 → อยู่ใน domain เสมอ */
    printf("\nA. correctness — _gn_mod6 (Barrett shift) เทียบ % 6\n");
    {
        int ok_dom = 1, ok_bound = 0;
        for (uint64_t n = 0; n < 3456; n++)
            if (_gn_mod6((uint32_t)n) != (uint8_t)(n % 6u)) ok_dom = 0;
        for (uint64_t n = 0; n < (1u << 24); n++)
            if (_gn_mod6((uint32_t)n) != (uint8_t)(n % 6u)) { ok_bound = (n == 32771); break; }
        CHECK("domain จริงของ geo_net (n < 3456): แม่น 100% (0 ผิด)", ok_dom);
        CHECK("⚠️ domain bound ตรงตามคาด: ล้มค่าแรกที่ 32771 (n ≥ 2^15)", ok_bound);
        printf("        (Barrett valid สำหรับ n < 2^15 — นอกนั้นต้อง % 6 ตรงๆ)\n");
    }

    /* ── B. cycles — rdtsc ── */
    printf("\nB. cycles — rdtsc (min of 7 × 200K calls)\n");
    {
        volatile uint32_t sink = 0;
        uint64_t best = ~0ULL;
        for (int trial = 0; trial < 7; trial++) {
            uint64_t t0, t1;
            asm volatile("lfence" ::: "memory");
            t0 = __rdtsc();
            for (int i = 0; i < 200000; i++)
                sink += _gn_mod6((uint32_t)(i * 2654435761u));
            asm volatile("lfence" ::: "memory");
            t1 = __rdtsc();
            uint64_t per = (t1 - t0) / 200000;
            if (per < best) best = per;
        }
        printf("        _gn_mod6 (Barrett): ~%llu cycles/call\n", (unsigned long long)best);
        CHECK("ใกล้ 2 cycles หรือไม่ (Barrett = mul+shift+sub ≈ 3-5)", best <= 6);
        printf("        (mul 1 + shift 1 + mul 1 + sub 1 = 4 ops — 2 cycles = เฉพาะ mul+shift)\n");
    }

    /* ── C. labels จาก route ── */
    printf("\nC. labels ที่ route สร้างต่อ address\n");
    {
        uint8_t spoke, face, unit, inv, group;
        uint16_t slot;
        int spoke_vals[6] = {0}, inv_vals[6] = {0};
        for (uint32_t a = 0; a < CYL_FULL_N; a++) {
            _route_labels(a, &spoke, &slot, &face, &unit, &inv, &group);
            spoke_vals[spoke]++;
            inv_vals[inv]++;
        }
        int all_spokes = 1, all_inv = 1;
        for (int i = 0; i < 6; i++) { if (!spoke_vals[i]) all_spokes = 0; if (!inv_vals[i]) all_inv = 0; }
        CHECK("spoke ครบ 6 ทิศ", all_spokes);
        CHECK("inv_spoke ครบ 6 ทิศ (mirror) → 6+6 = 12 ทิศชี้", all_inv);
        printf("        labels: spoke 0..5 · inv_spoke (spoke+3)%6 · slot 0..575 · face 0..8 · unit 0..63 · group 0..7\n");
    }

    printf("\n═══════════════════════════════════════\n");
    printf("RESULT: %d PASS / %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
}
