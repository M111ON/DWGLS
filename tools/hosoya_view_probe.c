/* tools/hosoya_view_probe.c — ภาษาที่ 4: golden-spiral (phyllotaxis) view
 * ═══════════════════════════════════════════════════════════════════════════
 * circle-packing family → sunflower/phyllotaxis order → golden angle =
 * Fibonacci convergent → discrete stride บน 60 RID slots:
 *
 *   view_hosoya[j] = (j · 13) mod 60
 *
 * ที่มาของ 13 (ไม่ใช่เลขสุ่ม):
 *   - 13 = F(7) ใน Fibonacci ladder (1 1 2 3 5 8 13 21 ...)
 *   - 13 เป็น Hosoya cell จริง: T(6,0) = F(1)·F(7) = 13
 *   - convergent 13/8 ≈ φ (golden angle ของ phyllotaxis/circle packing)
 *   - gcd(13, 60) = 1 → multiplication by 13 mod 60 เป็น permutation
 *     (oracle: Euclid — อิสระจาก implementation)
 *   - inverse: 13·37 = 481 = 8·60 + 1 → 37 = 13⁻¹ (mod 60)
 *     pattern เดียวกับ magnify glass: a_w × a_{w+72} ≡ 1 (mod 144)
 *
 * พิสูจน์:
 *   O1 gcd(13,60)=1 (Euclid algorithm — oracle อิสระ)
 *   O2 {j·13 mod 60 : j=0..59} ครบ 60 ค่า ไม่ซ้ำ (bijection จริง)
 *   O3 inverse roundtrip: ((j·13)·37) mod 60 == j ทุก j
 *   O4 13 อยู่ใน Hosoya triangle rows ≤ 12 หลาย cell (T(6,0), T(6,6), T(12,0)..)
 *   O5 13/8 ≈ φ (|err| < 0.05) — golden-angle derivation
 *   O6 ladder: F(7)=13, F(12)=144 (สนามเดิมของระบบ)
 *   M1 mutation: stride 14 (gcd=2) ต้อง FAIL distinctness — suite fail ได้จริง
 *
 * BUILD: gcc -O2 -Wall -I core -o build/hosoya_view_probe tools/hosoya_view_probe.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SLOTS 60
#define STRIDE 13u

static int pass = 0, fail = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass++; printf("  PASS — %s\n", desc); } \
    else      { fail++; printf("  FAIL — %s\n", desc); } \
} while(0)

static uint64_t gcd64(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

static uint64_t F[24];
static uint64_t hosoya(int n, int k) { return F[k + 1] * F[n - k + 1]; }

int main(void) {
    printf("=== hosoya_view_probe — golden-spiral view on 60 RID slots ===\n");

    /* ── O1. Euclid oracle ── */
    printf("\nO1. gcd oracle (Euclid)\n");
    CHECK("gcd(13, 60) = 1 → stride-13 multiplication mod 60 = permutation",
          gcd64(STRIDE, SLOTS) == 1);
    printf("        Euclid: gcd(%llu,%llu) = %llu\n",
           (unsigned long long)STRIDE, (unsigned long long)SLOTS,
           (unsigned long long)gcd64(STRIDE, SLOTS));

    /* ── O2. empirical bijection ── */
    printf("\nO2. bijection จริงบน 60 slots\n");
    {
        uint8_t hit[SLOTS]; memset(hit, 0, sizeof(hit));
        int distinct = 1;
        for (uint32_t j = 0; j < SLOTS; j++) {
            uint32_t w = (j * STRIDE) % SLOTS;
            if (hit[w]) distinct = 0;
            hit[w] = 1;
        }
        int covered = 1;
        for (int w = 0; w < SLOTS; w++) if (!hit[w]) covered = 0;
        CHECK("view_hosoya[j]=(j*13)%%60 injective (ไม่มี collision)", distinct == 1);
        CHECK("surjective (ครบทุก slot 0..59)", covered == 1);
    }

    /* ── O3. inverse roundtrip ── */
    printf("\nO3. inverse 13^-1 = 37 (mod 60)\n");
    {
        int rt_ok = 1;
        for (uint32_t j = 0; j < SLOTS; j++) {
            uint32_t w = (j * STRIDE) % SLOTS;
            uint32_t back = (w * 37u) % SLOTS;
            if (back != j) rt_ok = 0;
        }
        CHECK("((j*13)*37) mod 60 == j ทุก j (lossless roundtrip)", rt_ok == 1);
        CHECK("13*37 = 481 = 8*60 + 1 (inverse identity)",
              (uint64_t)STRIDE * 37u == 481ull && 481ull % SLOTS == 1ull);
        printf("        pattern เดียวกับ magnify glass: a_w * a_inv == 1 (mod n)\n");
    }

    /* ── O4/O5/O6. ที่มาของ 13 ── */
    printf("\nO4. 13 มาจาก Hosoya/Fibonacci (ไม่ใช่ magic number)\n");
    F[0] = 0; F[1] = 1; F[2] = 1;
    for (int i = 3; i < 24; i++) F[i] = F[i-1] + F[i-2];
    {
        int cells = 0;
        for (int n = 0; n <= 12; n++)
            for (int k = 0; k <= n; k++)
                if (hosoya(n, k) == 13) cells++;
        CHECK("T(6,0) = F(1)*F(7) = 13 (Hosoya cell จริง)", hosoya(6, 0) == 13);
        CHECK("13 ปรากฏ >= 3 cells ใน rows<=12 (symmetry ของ triangle)", cells >= 3);
        CHECK("F(7) = 13 (ladder)", F[7] == 13);
    }
    {
        double phi = 1.618033988749895;
        double r = (double)F[7] / (double)F[6];
        CHECK("|13/8 - phi| < 0.05 (golden-angle convergent)",
              (r > phi - 0.05) && (r < phi + 0.05));
        printf("        13/8 = %.4f vs phi = %.4f\n", r, phi);
    }
    CHECK("F(12) = 144 (สนามเดิมของระบบ — ladder ต่อเนื่อง)", F[12] == 144);

    /* ── M1. mutation check ── */
    printf("\nM1. mutation check — suite ต้อง fail ได้จริง\n");
    {
        uint8_t hit[SLOTS]; memset(hit, 0, sizeof(hit));
        int distinct14 = 1;
        for (uint32_t j = 0; j < SLOTS; j++) {
            uint32_t w = (j * 14u) % SLOTS;   /* mutant stride */
            if (hit[w]) distinct14 = 0;
            hit[w] = 1;
        }
        CHECK("stride 14 (gcd=2) ชนกันเอง → bijection แตก (mutation red)",
              distinct14 == 0);
    }

    printf("\nRESULT: %d PASS / %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
}
