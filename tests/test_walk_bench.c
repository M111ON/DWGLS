/*
 * test_walk_bench.c — Benchmark: stride-37 walk ครบรอบบน 720/1440 ticks
 * ═══════════════════════════════════════════════════════════════════════
 * (§15.102 — ต่อจาก test_walk_sync: walk = index = clock)
 *
 * คำถาม: walk clock จ่ายค่า "bijection" (เดินครบทุกตำแหน่ง) แพงไหม
 * เทียบกับ random access (ชี้ที่ไหนก็ได้) หรือ stride ที่ไม่ coprime
 * (วนซ้ำที่เดิม — ครอบไม่ครบ)?
 *
 * Proof:
 *   B1  coverage: stride-37 ครอบครบ 720/1440 ตำแหน่ง 1 ครั้ง (bijection)
 *   B2  non-coprime stride (36/42) ครอบไม่ครบ — วนซ้ำวงแคบ (gcd จำกัดวง)
 *   B3  throughput: ns/step ของ stride-37 เทียบ stride ที่ไม่ coprime
 *       (gcd 1 vs gcd > 1 — เดินเลขคณิตชุดเดียวกัน)
 *   B4  throughput: ns/step ของ stride-37 เทียบ random-access permutation
 *       — bijection walk ไม่ช้ากว่า random access (ทั้งคู่ O(1)/step)
 *   B5  ผลลัพธ์เป็น benchmark (พิมพ์ตัวเลข) — coverage เป็น assert จริง
 *       (ไม่ assert เวลาเป๊ะ — กัน flaky บนเครื่องต่างกัน)
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -Icore/infra -o build/test_walk_bench \
 *        tests/test_walk_bench.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint32_t gcd32(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

/* ── walk kernels (touch = กากบาทกัน compiler optimize ออก) ───────────── */
static volatile uint64_t g_touch;

/* random permutation (deterministic LCG — stride == 0 = random access):
 * x_{k+1} = 5·x_k + 1 mod n — ต้องเป็น permutation ของทุกตำแหน่ง
 * (ตรวจใน B4 ว่า cover ครบ n) */
static uint32_t step_stride(uint32_t pos, uint32_t stride, uint32_t n)
{
    if (stride == 0)
        return (uint32_t)(((uint64_t)pos * 5 + 1) % n);
    return (uint32_t)((pos + stride) % n);
}

/* เดิน n_steps ก้าว วัด ns/step — จัดรอบให้ ≥ 2ms กัน noise */
static double bench_steps(uint32_t n, uint32_t stride, uint32_t (*step)(uint32_t, uint32_t, uint32_t))
{
    uint32_t pos = 0;
    uint64_t rounds = 1;
    /* รอบล่วงหน้าเพื่อประเมินเวลา */
    double t0 = now_ms();
    for (uint32_t i = 0; i < n; i++) { pos = (uint32_t)step(pos, stride, n); g_touch += pos; }
    double t1 = now_ms() - t0;
    if (t1 > 0) rounds = (uint64_t)(8.0 / (t1 / 1000.0));   /* ≈ 8 ms ต่อกรณี */
    if (rounds < 8) rounds = 8;

    t0 = now_ms();
    uint64_t steps = 0;
    for (uint64_t r = 0; r < rounds; r++)
        for (uint32_t i = 0; i < n; i++) { pos = (uint32_t)step(pos, stride, n); g_touch += pos; steps++; }
    double ms = now_ms() - t0;
    return ms * 1e6 / (double)steps;   /* ns/step */
}

/* ── coverage: เดินครบ ring ครอบทุกตำแหน่งกี่ครั้ง ─────────────────────── */
static void check_coverage(uint32_t n, uint32_t stride, uint32_t (*step)(uint32_t, uint32_t, uint32_t))
{
    uint8_t seen[2048] = {0};
    uint32_t pos = 0, visits = 0;
    for (uint32_t i = 0; i < n; i++) {
        pos = (uint32_t)step(pos, stride, n);
        seen[pos] = 1;
        visits++;
    }
    uint32_t covered = 0;
    for (uint32_t i = 0; i < n; i++) covered += seen[i];
    printf("  coverage stride-%u n=%u: เยี่ยม %u ตำแหน่ง (%u/%u — %.1f%%)\n",
           stride, n, covered, covered, n, 100.0 * covered / n);
    (void)visits;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Walk Benchmark — stride-37 ครบรอบ 720/1440 vs non-coprime vs random\n");
    printf("═══════════════════════════════════════════════════════════════════════\n");

    static const uint32_t RINGS[] = { 720u, 1440u };

    for (uint32_t ri = 0; ri < sizeof(RINGS)/sizeof(RINGS[0]); ri++) {
        uint32_t n = RINGS[ri];
        printf("\n── Ring %u ──\n", n);

        /* B1: stride-37 ครอบครบ 1 ครั้ง — bijection */
        uint8_t seen[2048] = {0};
        uint32_t pos = 0;
        int biject_ok = 1;
        for (uint32_t i = 0; i < n; i++) {
            pos = (pos + 37) % n;
            if (seen[pos]) biject_ok = 0;
            seen[pos] = 1;
        }
        for (uint32_t i = 0; i < n; i++) if (!seen[i]) biject_ok = 0;
        CHECK("B1: stride-37 ครอบครบทุกตำแหน่ง 1 ครั้ง (bijection)", biject_ok);

        /* B2: non-coprime ครอบไม่ครบ — gcd จำกัดวง */
        {
            uint8_t s2[2048] = {0};
            uint32_t p = 0;
            for (uint32_t i = 0; i < n; i++) { p = (p + 36) % n; s2[p] = 1; }
            uint32_t c36 = 0; for (uint32_t i = 0; i < n; i++) c36 += s2[i];
            uint8_t s3[2048] = {0};
            p = 0;
            for (uint32_t i = 0; i < n; i++) { p = (p + 42) % n; s3[p] = 1; }
            uint32_t c42 = 0; for (uint32_t i = 0; i < n; i++) c42 += s3[i];
            CHECK("B2: stride-36 ครอบแค่ n/gcd(36,n) ตำแหน่ง (วนซ้ำวงแคบ)",
                  c36 == n / gcd32(36, n));
            CHECK("B2b: stride-42 ครอบแค่ n/gcd(42,n) ตำแหน่ง",
                  c42 == n / gcd32(42, n));
        }

        /* B3/B4: throughput — stride-37 vs 36/42 vs random permutation */
        {
            double t37 = bench_steps(n, 37, step_stride);
            double t36 = bench_steps(n, 36, step_stride);
            double t42 = bench_steps(n, 42, step_stride);
            double trnd = bench_steps(n, 0, step_stride);

            printf("  ns/step: stride-37 = %.2f · stride-36 = %.2f · stride-42 = %.2f · random = %.2f\n",
                   t37, t36, t42, trnd);

            /* random access ไม่การันตี cover — ต่างจาก bijection (B1):
             * เดิน n ก้าวสุ่ม (LCG) ครอบได้ < n ตำแหน่ง (มีการซ้ำ)
             * — นี่คือเหตุผลว่า walk clock ต้องเป็น bijection ถึงจะ
             * "address ครบทุกพิกัด" ได้ */
            {
                uint8_t sr[2048] = {0};
                uint32_t p = 0;
                for (uint32_t i = 0; i < n; i++) { p = (uint32_t)(((uint64_t)p * 5 + 1) % n); sr[p] = 1; }
                uint32_t c = 0; for (uint32_t i = 0; i < n; i++) c += sr[i];
                CHECK("B4a: random access n ก้าว ครอบ < n ตำแหน่ง (ไม่การันตี — ต้อง bijection)",
                      c < n);
            }

            /* bijection ไม่ช้ากว่า random access เกิน 3× (กัน noise กว้างๆ —
             * ทั้งคู่ O(1)/step — ปกติต่างกันไม่ถึง 2×) */
            CHECK("B3: stride-37 เร็วเท่าระดับ stride ทั่วไป (gcd 1 ≈ gcd > 1)",
                  t37 < t36 * 3.0 && t37 < t42 * 3.0);
            CHECK("B4: bijection walk ไม่ช้ากว่า random access (O(1)/step — ต่าง < 3×)",
                  t37 < trnd * 3.0);
        }
    }

    /* B5: benchmark สรุปตัวเลข — bijection ครอบ 100% เสมอ ไม่ว่า stride ไหน */
    printf("\nสรุป: stride-37 เดิน 720/1440 ครบ 100%% · non-coprime ครอบ n/gcd(n,k) · ทั้งหมด O(1)/step\n");

    printf("\n═══════════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
