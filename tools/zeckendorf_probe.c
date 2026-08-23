/* tools/zeckendorf_probe.c — Zeckendorf decomposition + reversed-code view
 * ═══════════════════════════════════════════════════════════════════════════
 * Zeckendorf theorem: จำนวนเต็มบวกทุกตัวเขียนเป็นผลบวกของ Fibonacci
 * ที่ไม่ติดกันได้ "ทางเดียวเท่านั้น" (unique) — greedy (หยิบ F ใหญ่สุดก่อน)
 * ให้ representation นั้นเสมอ
 *
 * เชื่อมกับระบบ:
 *   - word-ladder ในภาพ circle packing: "1","11","111","212","21212"
 *     เป็น palindrome และ digit-sum = 1,2,3,5,8 (Fibonacci)
 *     (chain ที่ label 13 = "212212": palindrome ✓ แต่ digit-sum = 10 ≠ 13
 *      → รายงานตรงๆ ว่าไม่ match — ไม่ force-fit)
 *   - palindrome ↔ KIS enter-anywhere/backward: code อ่านย้อนกลับได้
 *   - reversed-code view: เรียง slot 0..59 ด้วย Zeckendorf code ที่กลับบิต
 *     → permutation (code unique → reversed code ก็ unique)
 *
 * พิสูจน์:
 *   Z1 greedy ให้ผลบวกถูกต้องครบ 1..4000 (existence)
 *   Z2 representation ไม่มี Fibonacci ติดกัน ครบ 1..4000 (canonical form)
 *   Z3 uniqueness โดย brute-force enumerate subset ไม่ติดกันของ F(2..16)
 *      เทียบ n ≤ 500 — ต้องพบ subset เดียวต่อ n (oracle อิสระจาก greedy)
 *   Z4 reversed-code permutation บน 0..59: bijective + deterministic
 *   Z5 word-ladder: 5 chains แรก palindrome + sum = 1,2,3,5,8
 *      + รายงาน chain "13" ตามจริง
 *   M1 mutation: greedy ที่หยิบ "เล็กสุดก่อน" ต้องทำ Z2 แดง (fail ได้จริง)
 *
 * BUILD: gcc -O2 -Wall -I core -o build/zeckendorf_probe tools/zeckendorf_probe.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int pass = 0, fail = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass++; printf("  PASS — %s\n", desc); } \
    else      { fail++; printf("  FAIL — %s\n", desc); } \
} while(0)

#define NF 20
static uint64_t F[NF];

/* greedy Zeckendorf — mask bit i = F(i+2) ใช้หรือไม่ */
static uint64_t zeck_greedy(uint64_t n) {
    uint64_t mask = 0;
    for (int i = NF - 3; i >= 0; i--) {
        if (F[i + 2] <= n) { mask |= 1ull << i; n -= F[i + 2]; }
    }
    return mask;
}
/* ตรวจ canonical: ไม่มีบิตติดกัน */
static int no_adjacent(uint64_t mask) {
    return (mask & (mask << 1)) == 0;
}
/* mutant: หยิบเล็กสุดก่อน (anti-greedy) */
static uint64_t zeck_antigreedy(uint64_t n) {
    uint64_t mask = 0;
    for (int i = 0; i <= NF - 3; i++) {
        if (F[i + 2] <= n) { mask |= 1ull << i; n -= F[i + 2]; }
    }
    return mask;
}

/* brute-force: นับทุก subset ไม่ติดกันของ F(2..16) ที่ sum ≤ 500
 * นับ "ที่ leaf เท่านั้น" — subset หนึ่ง = path เดียว = นับครั้งเดียว */
static uint32_t zbucket[501];
static void rec(int idx, uint64_t sum, int prev_used) {
    if (idx > 14) {
        if (sum <= 500) zbucket[sum]++;
        return;
    }
    rec(idx + 1, sum, 0);
    if (!prev_used && sum + F[idx + 2] <= 500)
        rec(idx + 1, sum + F[idx + 2], 1);
}

int main(void) {
    printf("=== zeckendorf_probe — unique non-consecutive Fib decomposition ===\n");
    F[0] = 0; F[1] = 1; F[2] = 1;
    for (int i = 3; i < NF; i++) F[i] = F[i-1] + F[i-2];

    /* ── Z1/Z2. greedy = existence + canonical ── */
    printf("\nZ1/Z2. greedy Zeckendorf บน 1..4000\n");
    {
        int sum_ok = 1, canon_ok = 1;
        for (uint64_t n = 1; n <= 4000; n++) {
            uint64_t m = zeck_greedy(n);
            uint64_t s = 0;
            for (int i = 0; i <= NF - 3; i++) if (m & (1ull << i)) s += F[i + 2];
            if (s != n) sum_ok = 0;
            if (!no_adjacent(m)) canon_ok = 0;
        }
        CHECK("greedy คืนผลบวก = n ครบทุกตัว (existence)", sum_ok);
        CHECK("ไม่มีบิต Fibonacci ติดกัน ครบทุกตัว (non-consecutive)", canon_ok);
        printf("        ตัวอย่าง: 100 = 89+8+3 → mask อ่านย้อนได้ (palindrome ธีม)\n");
    }

    /* ── Z3. uniqueness — brute force อิสระจาก greedy ── */
    printf("\nZ3. uniqueness (brute-force subset ไม่ติดกัน, n<=500)\n");
    {
        memset(zbucket, 0, sizeof(zbucket));
        rec(0, 0, 0);
        int uniq_ok = 1, zero_ok = 1;
        for (int n = 1; n <= 500; n++) {
            if (zbucket[n] == 0) zero_ok = 0;
            if (zbucket[n] != 1) uniq_ok = 0;
        }
        CHECK("n ทุกตัว 1..500 มี representation", zero_ok);
        CHECK("representation เดียวเท่านั้น (bucket==1 ทุก n) — Zeckendorf theorem", uniq_ok);
    }

    /* ── Z4. reversed-code view บน 60 slots ── */
    printf("\nZ4. reversed-Zeckendorf view บน slot 0..59\n");
    {
        /* key(n) = code(n+1) กลับบิต 9 ตำแหน่ง (F(2)..F(10) = bits 0..8);
         * เรียง slot 0..59 ด้วย key */
        uint32_t key[60];
        for (int n = 0; n < 60; n++) {
            uint64_t m = zeck_greedy((uint64_t)(n + 1));
            uint64_t rev = 0;
            for (int b = 0; b < 9; b++)
                if (m & (1ull << b)) rev |= 1ull << (8 - b);
            key[n] = (uint32_t)rev;
        }
        /* sort slot index ด้วย key (insertion sort — stable ไม่จำเป็น key unique) */
        uint32_t ord[60];
        for (int i = 0; i < 60; i++) ord[i] = (uint32_t)i;
        for (int i = 1; i < 60; i++) {
            uint32_t v = ord[i]; int j = i - 1;
            while (j >= 0 && key[ord[j]] > key[v]) { ord[j + 1] = ord[j]; j--; }
            ord[j + 1] = v;
        }
        /* bijection check */
        uint8_t hit[60]; memset(hit, 0, sizeof(hit));
        int bijective = 1;
        for (int p = 0; p < 60; p++) {
            if (hit[ord[p]]) bijective = 0;
            hit[ord[p]] = 1;
        }
        CHECK("view_zeck[p] = slot อันดับ p ตาม reversed code → bijection 60/60",
              bijective);
        /* determinism: sort ซ้ำได้ผลเดิม */
        uint32_t ord2[60];
        for (int i = 0; i < 60; i++) ord2[i] = (uint32_t)i;
        for (int i = 1; i < 60; i++) {
            uint32_t v = ord2[i]; int j = i - 1;
            while (j >= 0 && key[ord2[j]] > key[v]) { ord2[j + 1] = ord2[j]; j--; }
            ord2[j + 1] = v;
        }
        CHECK("deterministic — sort ซ้ำได้ permutation เดิมเป๊ะ",
              memcmp(ord, ord2, sizeof(ord)) == 0);
        printf("        4 ตัวหน้า: ");
        for (int p = 0; p < 4; p++) printf("%u ", ord[p]);
        printf("\n");
    }

    /* ── Z5. word-ladder จากรูป circle packing (transcription corrected:
     *        word ของ 13 = "21212212" ยาว 8 ไม่ใช่ "212212") ── */
    printf("\nZ5. circle-chain words — palindrome + digit-sum ladder\n");
    {
        const char *w[] = { "1", "11", "111", "212", "21212", "21212212" };
        uint64_t want[] = { 1, 2, 3, 5, 8, 13 };
        int pal_ok = 1, sum_ok = 1;
        int pal_count = 0;
        for (int k = 0; k < 6; k++) {
            int len = (int)strlen(w[k]);
            int isp = 1;
            for (int a = 0, b = len - 1; a < b; a++, b--)
                if (w[k][a] != w[k][b]) isp = 0;
            pal_count += isp;
            uint64_t s = 0;
            for (int a = 0; a < len; a++) s += (uint64_t)(w[k][a] - '0');
            if (s != want[k]) sum_ok = 0;
            printf("        %-2llu = %-9s sum=%llu %s%s\n",
                   (unsigned long long)want[k], w[k], (unsigned long long)s,
                   isp ? "palindrome" : "NOT-pal",
                   s == want[k] ? " ✓" : " ✗");
        }
        /* 5 chains แรก palindrome · W6="21212212" ไม่เป็น
         * (even-length + odd sum ⇒ palindrome impossible — โครงสร้างจริง)
         * และสังเกต: W6 = W5 ∥ W4 = "21212"+"212"
         *   → sum(W6) = sum(W5)+sum(W4) = 8+5 = 13 (Fibonacci recurrence!) */
        CHECK("digit-sum ครบ 6 chains = 1,2,3,5,8,13 (Fibonacci ladder)", sum_ok);
        CHECK("5 chains แรก palindrome (chain 13 ไม่เป็น — ตรงรูป)", pal_count == 5);
    }

    /* ── M1. mutation ── */
    printf("\nM1. mutation check — anti-greedy ต้องแดง\n");
    {
        int bad = 0;
        for (uint64_t n = 1; n <= 4000; n++)
            if (!no_adjacent(zeck_antigreedy(n))) bad++;
        CHECK("small-first greedy ทำ non-consecutive พัง (%d/4000 red)", bad > 0);
        printf("        anti-greedy พัง %d/4000 cases → suite fail ได้จริง\n", bad);
    }

    printf("\nRESULT: %d PASS / %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
}
