/*
 * geo_jump_explore.c — สำรวจ geo_jump ทำอะไรได้บ้าง
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/geo_jump_explore tests/geo_jump_explore.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../FGLS_new/collection/geo_jump_module/include/geo_jump.h"

#define N 20736u

/* ═══════════════════════════════════════════════════════════════════════════
   Test 1: ทุก jump type — walk 10 steps จาก slot 0
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_jump_types(void) {
    printf("═══ Test 1: Jump Types — walk 10 steps from slot 0 ═══\n\n");

    const char *names[] = {"HILBERT", "PEANO", "PENTAGON", "MOD", "INVERT", "GROUND"};
    GeoJumpType types[] = {JUMP_HILBERT, JUMP_PEANO, JUMP_PENTAGON, JUMP_MOD, JUMP_INVERT, JUMP_GROUND};

    for (int t = 0; t < 6; t++) {
        printf("  [%s]\n", names[t]);
        uint32_t node = 0;
        printf("    ");
        for (int i = 0; i < 10; i++) {
            uint32_t next = geo_jump(node, types[t], 1);
            printf("%5u→", node);
            node = next;
        }
        printf("%5u\n", node);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 2: MOD — stride variations
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_mod_stride(void) {
    printf("═══ Test 2: MOD Stride — 5 (BEST), 37 (frame_seek), 162, 16813 ═══\n\n");

    uint32_t strides[] = {5, 37, 162, 16813, 1440, 576};
    const char *labels[] = {"5 (BEST, order 1728)", "37 (frame_seek)", "162 (tower identity)", "16813 (inverse)", "1440 (clock)", "576 (20736/36)"};

    for (int s = 0; s < 6; s++) {
        printf("  stride=%s:\n", labels[s]);
        uint32_t node = 0;
        printf("    ");
        for (int i = 0; i < 8; i++) {
            printf("%5u ", node);
            node = geo_jump(node, JUMP_MOD, strides[s]);
        }
        printf("\n\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 3: PENTAGON — jump ระหว่าง pentagon faces
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_pentagon(void) {
    printf("═══ Test 3: Pentagon — 12 faces, jump between them ═══\n\n");

    for (uint32_t face = 0; face < 12; face++) {
        uint32_t node = face * 1728; /* แต่ละ face = 1728 slots */
        uint32_t pent = geo_pentagon_id(node);
        uint32_t tick = geo_clock_tick(node);
        uint32_t shell = geo_shell_level(node);
        printf("  node=%5u → pentagon=%2u, tick=%4u, shell=%2u\n",
               node, pent, tick, shell);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 4: INVERT — bipolar mirror
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_invert(void) {
    printf("═══ Test 4: Invert — mirror within tower ═══\n\n");

    for (uint32_t i = 0; i < 48; i++) {
        uint32_t inv = geo_jump(i, JUMP_INVERT, 0);
        if (i < 10 || (i >= 44)) {
            printf("  [%3u] → [%3u]  (block=%u, local=%u → block=%u, local=%u)\n",
                   i, inv,
                   i / 16, i % 16,
                   inv / 16, inv % 16);
        }
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 5: CAPO — offset routing
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_capo(void) {
    printf("═══ Test 5: Capo — offset by key × tower ═══\n\n");

    uint32_t base = 100;
    uint32_t keys[] = {0, 1, 2, 5, 10, 12};

    printf("  base=%u:\n", base);
    for (int k = 0; k < 6; k++) {
        uint32_t shifted = geo_capo(base, keys[k]);
        printf("    key=%2u → slot=%5u (offset=%+5d)\n",
               keys[k], shifted, (int)shifted - (int)base);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 6: DNA — walk + timeline
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_dna(void) {
    printf("═══ Test 6: DNA — walk + timeline ═══\n\n");

    GeoJumpRouter router = {JUMP_HILBERT, 1, 0, 0};
    GeoJumpWalk walk;
    geo_walk_init(&walk, 0, &router);

    /* Walk 20 steps */
    for (int i = 0; i < 20; i++) geo_walk_step(&walk);

    GeoDna dna;
    geo_dna_from_walk(&dna, &walk, 0);

    printf("  Walk start=%u, end=%u, steps=%u\n",
           dna.head, dna.tail, dna.length);

    /* Timeline: 12 layers */
    uint32_t timeline[12];
    geo_dna_timeline_all(&dna, timeline);

    printf("  Timeline (12 layers):\n    ");
    for (int i = 0; i < 12; i++) {
        printf("%5u ", timeline[i]);
    }
    printf("\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 7: Climate — zone classification
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_climate(void) {
    printf("═══ Test 7: Climate — zone from field ═══\n\n");

    uint32_t nodes[] = {0, 10, 24, 100, 200, 500, 1000, 5000, 10000, 20000};
    const char *zones[] = {"TROPICAL", "TEMPERATE", "BOREAL", "TUNDRA"};

    printf("  %-8s %-12s %-10s %-10s\n", "Node", "Zone", "Pentagon", "Shell");
    printf("  %-8s %-12s %-10s %-10s\n", "────", "────", "───────", "─────");

    for (int i = 0; i < 10; i++) {
        GeoFieldClimate z = geo_field_climate(nodes[i], 0);
        printf("  [%5u]  zone=%-12u [%2u]      [%2u]\n",
               nodes[i],
               z.zone,
               geo_pentagon_id(nodes[i]),
               geo_shell_level(nodes[i]));
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 8: Full walk — ทุก slot ได้ address ไหม? (coverage)
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_coverage(void) {
    printf("═══ Test 8: Coverage — HILBERT walk ทุก slot ═══\n\n");

    uint8_t visited[N];
    memset(visited, 0, sizeof(visited));

    uint32_t node = 0;
    uint32_t max_step = 0;

    for (uint32_t i = 0; i < N; i++) {
        if (visited[node]) {
            max_step = i;
            break;
        }
        visited[node] = 1;
        node = geo_jump(node, JUMP_HILBERT, 1);
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < N; i++) count += visited[i];

    printf("  HILBERT: %u/%u unique slots (%.1f%%)\n",
           count, N, 100.0 * count / N);
    printf("  First repeat at step: %u\n\n", max_step);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 9: Router combo — chain multiple jumps
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_router_combo(void) {
    printf("═══ Test 9: Router Combo — chain HILBERT → MOD → INVERT ═══\n\n");

    GeoJumpRouter router = {JUMP_HILBERT, 1, 0, 0};
    uint32_t node = 0;

    printf("  Start: %u\n", node);
    for (int i = 0; i < 5; i++) {
        /* Hilbert step */
        node = geo_jump(node, JUMP_HILBERT, 1);
        printf("  HILBERT → %u\n", node);

        /* Mod step */
        node = geo_jump(node, JUMP_MOD, 37);
        printf("  MOD(37) → %u\n", node);

        /* Invert step */
        node = geo_jump(node, JUMP_INVERT, 0);
        printf("  INVERT → %u\n", node);

        printf("  ---\n");
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Geo Jump Explorer — ทำอะไรได้บ้าง?                      ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    test_jump_types();
    test_mod_stride();
    test_pentagon();
    test_invert();
    test_capo();
    test_dna();
    test_climate();
    test_coverage();
    test_router_combo();

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  SUMMARY\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  6 jump types: HILBERT, PEANO, PENTAGON, MOD, INVERT, GROUND\n");
    printf("  Customizable: type, param, param2, param3\n");
    printf("  Chainable: any sequence of jumps\n");
    printf("  DNA: walk → timeline (12 layers)\n");
    printf("  Climate: 4 zones (TROPICAL/TEMPERATE/BOREAL/TUNDRA)\n");
    printf("  Capo: offset routing by key\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}
