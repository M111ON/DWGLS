/*
 * geo_jump_real_test.c — test geo_jump จริงจัง (ไม่ใช่ filling)
 *
 * Hilbert/Peano = maze walls (structure stays still, data moves)
 * MOD = stride multiplication
 * INVERT = mirror
 * PENTAGON = face routing
 *
 * BUILD: gcc -O2 -Wall -DGEO_JUMP_INLINE -I../../FGLS_new/collection/geo_jump_module/include -o build/geo_jump_real tests/geo_jump_real_test.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../FGLS_new/collection/geo_jump_module/include/geo_jump.h"

#define N 20736u

/* ═══════════════════════════════════════════════════════════════════════════
   Test 1: MOD — start จาก non-zero slots
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_mod_nonzero(void) {
    printf("═══ Test 1: MOD from non-zero slots ═══\n\n");

    uint32_t starts[] = {1, 100, 1000, 5000, 10000};
    uint32_t strides[] = {27, 34, 17, 54, 162, 16813};

    for (int s = 0; s < 6; s++) {
        printf("  stride=%u:\n", strides[s]);
        for (int st = 0; st < 3; st++) {
            uint32_t node = starts[st];
            printf("    start=%5u: ", node);
            for (int i = 0; i < 8; i++) {
                node = geo_jump(node, JUMP_MOD, strides[s]);
                printf("%5u ", node);
            }
            printf("\n");
        }
        printf("\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 2: MOD coverage — 37 stride ครบทุก slot ไหม?
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_mod_coverage(void) {
    printf("═══ Test 2: MOD Coverage — ทุก stride เทียบ 20736 ═══\n\n");

    uint32_t strides[] = {27, 34, 17, 54, 162, 16813};
    const char *labels[] = {"27 (trit)", "34 (L38 SV)", "17 (8+9)", "54 (bridge)", "162 (ico)", "16813 (inv37)"};

    for (int s = 0; s < 6; s++) {
        uint8_t visited[N];
        memset(visited, 0, sizeof(visited));

        uint32_t node = 1;
        uint32_t steps = 0;
        uint32_t first_repeat = 0;

        for (uint32_t i = 0; i < N; i++) {
            if (visited[node]) {
                first_repeat = i;
                break;
            }
            visited[node] = 1;
            node = geo_jump(node, JUMP_MOD, strides[s]);
            steps++;
        }

        uint32_t count = 0;
        for (uint32_t i = 0; i < N; i++) count += visited[i];

        printf("  %-16s (%2u): %5u/%u unique (%.1f%%)  repeat@%u\n",
               labels[s], strides[s], count, N, 100.0*count/N, first_repeat);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 3: HILBERT — start จาก non-zero, ดู routing behavior
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_hilbert_nonzero(void) {
    printf("═══ Test 3: HILBERT from non-zero ═══\n\n");

    uint32_t starts[] = {1, 10, 100, 500, 1000, 5000, 10000};

    for (int st = 0; st < 7; st++) {
        uint32_t node = starts[st];
        printf("  start=%5u: ", node);
        for (int i = 0; i < 8; i++) {
            node = geo_jump(node, JUMP_HILBERT, 1);
            printf("%5u ", node);
        }
        printf("\n");
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 4: INVERT — cycle analysis
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_invert_cycle(void) {
    printf("═══ Test 4: INVERT — cycle analysis ═══\n\n");

    uint32_t starts[] = {0, 1, 10, 48, 96, 143};

    for (int st = 0; st < 6; st++) {
        uint32_t start = starts[st];
        uint32_t node = start;
        printf("  start=%3u: ", start);
        for (int i = 0; i < 12; i++) {
            node = geo_jump(node, JUMP_INVERT, 0);
            printf("%3u ", node);
        }
        printf("\n");
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 5: Chain — HILBERT → MOD → INVERT → MOD → HILBERT
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_chain(void) {
    printf("═══ Test 5: Chain combos ═══\n\n");

    uint32_t starts[] = {1, 100, 1000, 5000};

    for (int st = 0; st < 4; st++) {
        uint32_t node = starts[st];
        printf("  start=%5u:\n", node);
        for (int i = 0; i < 6; i++) {
            uint32_t prev = node;
            node = geo_jump(node, JUMP_HILBERT, 1);
            printf("    HILBERT: %5u → %5u\n", prev, node);

            prev = node;
            node = geo_jump(node, JUMP_MOD, 37);
            printf("    MOD(37): %5u → %5u\n", prev, node);

            prev = node;
            node = geo_jump(node, JUMP_INVERT, 0);
            printf("    INVERT:  %5u → %5u\n", prev, node);
        }
        printf("\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 6: Capo offset — key values
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_capo_full(void) {
    printf("═══ Test 6: Capo offset ═══\n\n");

    uint32_t base = 100;
    printf("  base=%u:\n", base);
    printf("  %-6s %-8s %-10s\n", "key", "slot", "offset");
    printf("  %-6s %-8s %-10s\n", "---", "----", "------");

    for (uint32_t k = 0; k <= 20; k++) {
        uint32_t s = geo_capo(base, k);
        printf("  [%2u]   [%5u]  [%+5d]\n", k, s, (int)s - (int)base);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 7: PENTAGON — all 12 faces
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_pentagon_full(void) {
    printf("═══ Test 7: Pentagon — jump between faces ═══\n\n");

    for (uint32_t face = 0; face < 12; face++) {
        uint32_t base = face * 1728;
        uint32_t pent = geo_pentagon_id(base);
        uint32_t tick = geo_clock_tick(base);
        uint32_t shell = geo_shell_level(base);

        /* jump to next pentagon */
        uint32_t next = geo_jump(base, JUMP_PENTAGON, 0);
        uint32_t next_pent = geo_pentagon_id(next);

        printf("  face=%2u: base=%5u pent=%2u tick=%4u shell=%2u → next pent=%2u\n",
               face, base, pent, tick, shell, next_pent);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 8: HILBERT coverage — start จากหลายจุด
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_hilbert_coverage(void) {
    printf("═══ Test 8: HILBERT coverage ═══\n\n");

    uint32_t starts[] = {1, 10, 100, 500, 1000, 5000, 10000, 19999};

    for (int st = 0; st < 8; st++) {
        uint8_t visited[N];
        memset(visited, 0, sizeof(visited));

        uint32_t start = starts[st];
        uint32_t node = start;
        uint32_t steps = 0;

        for (uint32_t i = 0; i < N; i++) {
            if (visited[node]) break;
            visited[node] = 1;
            node = geo_jump(node, JUMP_HILBERT, 1);
            steps++;
        }

        uint32_t count = 0;
        for (uint32_t i = 0; i < N; i++) count += visited[i];

        printf("  start=%5u: %u unique, %u steps before repeat\n",
               start, count, steps);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Geo Jump Real Test — หาปัญหาจริง                       ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    test_mod_nonzero();
    test_mod_coverage();
    test_hilbert_nonzero();
    test_invert_cycle();
    test_chain();
    test_capo_full();
    test_pentagon_full();
    test_hilbert_coverage();

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  ดูผลแล้วตัดสินใจ\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}
