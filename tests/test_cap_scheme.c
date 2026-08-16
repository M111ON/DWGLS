/* test_cap_scheme.c — Adaptive placement scheme chooser
 * ═══════════════════════════════════════════════════════════════════════════
 * Question (user): ไฟล์เริ่มเยอะ → ค่าแรกเข้า (chunk แรก w=0 ราคาเต็ม) ทุก
 * ไฟล์ → ไม่คุ้ม → สลับไป chain แบบ global + targeted (§15.33)
 *
 *   T1  per-file cost formula: 1-chunk file = full size (ค่าแรกเข้า 1152
 *       ใน fp model / เต็ม size ใน size model); n=144 file = 6 field ranks
 *   T2  single big file → both schemes ≈ equal → stays PER_FILE (locality)
 *   T3  many tiny files → per-file ≫ global → switches to GLOBAL
 *   T4  global ≤ per-file เสมอ (targeted เป็น optimal assignment)
 *   T5  determinism — เรียกซ้ำ → ผลเดิม
 *   T6  margin: switch เฉพาะเมื่อประหยัด ≥ margin (threshold ทำงาน)
 *   T7  ผสม: 5 big + 100 tiny — เลือกตามต้นทุนจริง (คำนวณทั้งคู่)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-cap_scheme tests/test_cap_scheme.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_placement_choose.h"
#include "../core/geo_ghost_envelope.h"

#define CHUNK 16384ull   /* slots per chunk (1 byte = 1 slot) */
#define K_MAX 5u         /* gate 1.0 */

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* build the global chunk-size array for a list of (n_chunks) files */
static uint64_t *build_global_sizes(const uint32_t *files_n, uint32_t n_files,
                                    uint32_t *N_out) {
    uint32_t N = 0;
    for (uint32_t f = 0; f < n_files; f++) N += files_n[f];
    uint64_t *s = (uint64_t *)malloc(N * sizeof(uint64_t));
    uint32_t k = 0;
    for (uint32_t f = 0; f < n_files; f++)
        for (uint32_t c = 0; c < files_n[f]; c++)
            s[k++] = CHUNK;    /* full chunks (uniform for synthetic) */
    /* sort ascending */
    for (uint32_t i = 0; i < N; i++)
        for (uint32_t j = i + 1; j < N; j++)
            if (s[j] < s[i]) { uint64_t t = s[i]; s[i] = s[j]; s[j] = t; }
    *N_out = N;
    return s;
}

int main(void) {
    printf("Adaptive placement scheme — per-file vs global-targeted\n");
    printf("════════════════════════════════════════════════════════\n");

    /* T1 — per-file cost formula */
    {
        uint64_t c1 = pc_per_file_cost(1, CHUNK, K_MAX);
        uint64_t c144 = pc_per_file_cost(144, CHUNK, K_MAX);
        CHECK(1, "1-chunk file → full size (ค่าแรกเข้า w=0)", c1 == CHUNK);
        printf("     per-file: 1ch=%llu | 144ch=%llu | 1000ch=%llu\n",
               (unsigned long long)c1, (unsigned long long)c144,
               (unsigned long long)pc_per_file_cost(1000, CHUNK, K_MAX));
        CHECK(1, "144-chunk file → 6 field ranks (หนึ่งต่อ 144)",
              c144 == pc_view_of(CHUNK, 0) + pc_view_of(CHUNK, 4) +
                     pc_view_of(CHUNK, 3) + pc_view_of(CHUNK, 2) +
                     pc_view_of(CHUNK, 1) + pc_view_of(CHUNK, 5));
    }

    /* T2 — single big file: both ≈ equal → PER_FILE (locality) */
    {
        uint32_t one[1] = { 1000 };
        uint32_t N = 0;
        uint64_t *g = build_global_sizes(one, 1, &N);
        uint64_t pf = pc_per_file_cost(1000, CHUNK, K_MAX);
        uint64_t gl = pc_global_cost(g, N, K_MAX);
        free(g);
        printf("     single 1000-chunk file: per-file %llu | global %llu (ratio %.2f)\n",
               (unsigned long long)pf, (unsigned long long)gl,
               (double)pf / (double)gl);
        CHECK(2, "single file: per-file ≈ global (≤ 1.5× — margin)",
              pf <= gl + gl / 2);
        CHECK(2, "single file → chooses PER_FILE (locality kept)",
              pc_choose(pf, gl, 50) == PC_SCHEME_PER_FILE);
    }

    /* T3 — 1,000 tiny files (1 chunk each): entry fees dominate → GLOBAL */
    {
        uint32_t tiny[1000];
        for (uint32_t i = 0; i < 1000; i++) tiny[i] = 1;
        uint32_t N = 0;
        uint64_t *g = build_global_sizes(tiny, 1000, &N);
        uint64_t pf = 0;
        for (uint32_t f = 0; f < 1000; f++)
            pf += pc_per_file_cost(1, CHUNK, K_MAX);
        uint64_t gl = pc_global_cost(g, N, K_MAX);
        free(g);
        printf("     1000×1-chunk: per-file %llu | global %llu (ratio %.1f)\n",
               (unsigned long long)pf, (unsigned long long)gl,
               (double)pf / (double)gl);
        CHECK(3, "many tiny files → per-file ≫ global (ค่าแรกเข้า ×1000)",
              pf > gl * 10);
        CHECK(3, "many tiny files → chooses GLOBAL",
              pc_choose(pf, gl, 50) == PC_SCHEME_GLOBAL);
    }

    /* T4 — global ≤ per-file always (targeted = optimal) */
    {
        int ok = 1;
        uint32_t cases[5][2] = { {1,1}, {144,1}, {7,7}, {1000,1}, {64,64} };
        for (int c = 0; c < 5; c++) {
            uint32_t nf = cases[c][0], nc = cases[c][1];
            uint32_t *arr = (uint32_t *)malloc(nf * sizeof(uint32_t));
            for (uint32_t i = 0; i < nf; i++) arr[i] = nc;
            uint32_t N = 0;
            uint64_t *g = build_global_sizes(arr, nf, &N);
            uint64_t pf = 0;
            for (uint32_t i = 0; i < nf; i++) pf += pc_per_file_cost(nc, CHUNK, K_MAX);
            uint64_t gl = pc_global_cost(g, N, K_MAX);
            if (gl > pf) ok = 0;
            printf("     %u files × %u ch: per-file %llu | global %llu\n",
                   nf, nc, (unsigned long long)pf, (unsigned long long)gl);
            free(g); free(arr);
        }
        CHECK(4, "global ≤ per-file ทุกกรณี (targeted = optimal assignment)", ok);
    }

    /* T5 — determinism */
    {
        uint32_t mix[20];
        for (uint32_t i = 0; i < 20; i++) mix[i] = 1 + (i * 7) % 200;
        uint32_t N1 = 0, N2 = 0;
        uint64_t *g1 = build_global_sizes(mix, 20, &N1);
        uint64_t *g2 = build_global_sizes(mix, 20, &N2);
        uint64_t pf = 0;
        for (uint32_t i = 0; i < 20; i++) pf += pc_per_file_cost(mix[i], CHUNK, K_MAX);
        uint64_t gl1 = pc_global_cost(g1, N1, K_MAX);
        uint64_t gl2 = pc_global_cost(g2, N2, K_MAX);
        int s1 = pc_choose(pf, gl1, 50), s2 = pc_choose(pf, gl2, 50);
        free(g1); free(g2);
        CHECK(5, "deterministic: เรียกซ้ำ → cost + scheme เดิม",
              gl1 == gl2 && s1 == s2);
    }

    /* T6 — margin threshold works */
    {
        /* per-file = 2× global → margin 50 (need > 1.5×) → GLOBAL */
        CHECK(6, "per-file 2× global @margin 50 → GLOBAL",
              pc_choose(2000, 1000, 50) == PC_SCHEME_GLOBAL);
        /* per-file = 1.2× global → not worth → PER_FILE */
        CHECK(6, "per-file 1.2× global @margin 50 → PER_FILE (คุ้มไม่พอ)",
              pc_choose(1200, 1000, 50) == PC_SCHEME_PER_FILE);
        /* stricter margin 100 (need > 2×) → 2× not enough → PER_FILE */
        CHECK(6, "margin 100: 2× ยังไม่พอ → PER_FILE",
              pc_choose(2000, 1000, 100) == PC_SCHEME_PER_FILE);
    }

    /* T7 — mixed: 5 big (10,000 ch) + 100 tiny (1 ch) */
    {
        uint32_t mix[105];
        for (uint32_t i = 0; i < 5; i++) mix[i] = 10000;
        for (uint32_t i = 5; i < 105; i++) mix[i] = 1;
        uint32_t N = 0;
        uint64_t *g = build_global_sizes(mix, 105, &N);
        uint64_t pf = 0;
        for (uint32_t i = 0; i < 105; i++) pf += pc_per_file_cost(mix[i], CHUNK, K_MAX);
        uint64_t gl = pc_global_cost(g, N, K_MAX);
        free(g);
        int chosen = pc_choose(pf, gl, 50);
        printf("     5 big + 100 tiny: per-file %llu | global %llu → %s\n",
               (unsigned long long)pf, (unsigned long long)gl,
               chosen == PC_SCHEME_GLOBAL ? "GLOBAL" : "PER_FILE");
        CHECK(7, "mixed → เลือก scheme ที่ต้นทุนถูกกว่า (global ≤ per-file)",
              (chosen == PC_SCHEME_GLOBAL) == (pf > gl + gl / 2));
    }

    printf("\n════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
