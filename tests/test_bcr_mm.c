/*
 * test_bcr_mm.c — Multi-Map BCR verification
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <x86intrin.h>
#include "b_fs_base.h"
#include "bcr_mm.h"

int main(void) {
    printf("=== Multi-Map BCR (Inverted Index) ===\n");
    BcrMmTree tree;
    bcr_mm_init(&tree);

    /* Insert key 42 with 1000 doc addresses */
    uint32_t seed = 0x1234u;
    uint32_t lcg(uint32_t*s){ *s=*s*1664525u+1013904223u; return *s; }
    for (int i = 0; i < 1000; i++) {
        uint16_t addr = (uint16_t)(lcg(&seed) % BCR_MM_N);
        bcr_mm_insert(&tree, 42, addr);
    }
    /* Insert key 7 with 10 addresses */
    for (int i = 0; i < 10; i++) bcr_mm_insert(&tree, 7, (uint16_t)(100 + i));

    printf("Nodes: %u  Pool used: %u / %u\n", tree.node_count, tree.pool_count, BCR_MM_MAX_POOL);

    /* Search key 42 */
    static uint16_t out[2048];
    uint32_t cnt = bcr_mm_search(&tree, 42, out, 2048);
    printf("Search key=42: %u results (expect 1000)\n", cnt);

    /* Search key 7 */
    uint32_t cnt7 = bcr_mm_search(&tree, 7, out, 2048);
    printf("Search key=7: %u results (expect 10)\n", cnt7);

    /* Search missing key */
    uint32_t cnt0 = bcr_mm_search(&tree, 999, out, 2048);
    printf("Search key=999 (missing): %u results (expect 0)\n", cnt0);

    /* Intersection test: BCR list (key 42) vs small HNSW-like list */
    static uint16_t hnsw_list[100];
    for (int i = 0; i < 100; i++) hnsw_list[i] = (uint16_t)(lcg(&seed) % BCR_MM_N);
    /* make hnsw_list sorted? intersection doesn't require sorted (nested) */
    static uint16_t inter[256];
    uint32_t icnt = bcr_mm_intersect(out, cnt, hnsw_list, 100, inter, 256);
    printf("Intersection: %u (BCR 1000 x HNSW 100)\n", icnt);

    /* Correctness: all 1000 inserted addresses retrievable */
    /* Re-insert tracking */
    /* Simple: count matches */
    int ok = (cnt == 1000 && cnt7 == 10 && cnt0 == 0);
    printf("Correctness: %s\n", ok ? "PASS" : "FAIL");

    /* Height check */
    printf("Height: %d\n", bcr_mm_height(&tree));

    /* Pool bound check */
    printf("Pool bound: %s\n", (tree.pool_count <= BCR_MM_MAX_POOL) ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}