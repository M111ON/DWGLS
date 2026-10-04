/* tests/test_frustum_descent.c — frustum descent chained exit->enter (spec #7166).
 *
 * Owner structure: quadtree 1<4<16<64 with 1 bound to capo; levels refine
 * downward; output/exit of the previous node = enter/input of the next.
 * fr spans grow with level (144<<level), so refinement runs level 3 -> 0
 * (coarse, capo scale -> fine). The chain advances by branch_path only
 * (<=5 slots/level, ~20 total << capo 1728), so the whole descent stays
 * inside ONE capo: the 1<->capo binding.
 *
 * Entry comes from the light index (the ANN-storage joint): top entry's
 * field_slot seeds the descent position, its hj_cluster picks the octant
 * view. Static ANN (horizontal) picks WHERE to enter; frustum descent
 * (vertical) refines WHAT span to serve.
 *
 * Gates:
 *   D1 order    : levels visit 3,2,1,0 (coarse -> fine)
 *   D2 binding  : capo_key identical on all 4 levels (1 <-> capo)
 *   D3 refine   : span_size strictly decreases along the descent
 *   D4 branches : every branch_path in [0,6) (6 directions live)
 *   D5 replay   : repeat descent -> identical route_refs (timeline replays)
 *   D6 adapter  : finest event resolves via frustum_route_to_memory_ref,
 *                 generation == 0, span_size > 0
 *
 * BUILD: gcc -O2 -I. -Icore -o build/test_frustum_descent tests/test_frustum_descent.c -lm
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/frustum_route.h"
#include "core/frustum_memory_adapter.h"
#include "core/geo_light_index.h"

#define NCHAIN 32

static int g_pass = 0, g_fail = 0;
static void check_ok(int ok, const char *name) {
    if (ok) { g_pass++; printf("  PASS  %s\n", name); }
    else    { g_fail++; printf("  FAIL  %s\n", name); }
}

static uint32_t rng = 0x5eedul;
static uint32_t next_u32(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

/* One full descent from a seed position/view. Fills ev[4] (levels 3..0). */
static int descend(uint32_t pos0, uint8_t view,
                   FrustumRouteEvent ev[4]) {
    FrRouteCache cache;
    fr_cache_init(&cache);
    uint32_t pos = pos0;
    for (int d = 3; d >= 0; d--) {
        FrustumSeeker s;
        s.position = pos % 20736u;
        s.view_id = view;
        s.voronoi_mask = 0xFFFFFFu;             /* full mask: geometry only */
        s.frustum_depth = (uint8_t)(d + 1);     /* depth d+1 -> level d */
        if (!fr_route_produce(&s, &cache, &ev[3 - d])) return 0;
        /* exit -> enter: advance by the branch taken (<=5 slots) */
        pos = (pos + ev[3 - d].branch_path) % 20736u;
    }
    return 1;
}

int main(void) {
    printf("=== FRUSTUM DESCENT (exit->enter, 1<->capo) ===\n");

    /* light-index entries as descent seeds (the joint, fixtures here) */
    static LIXEntry entries[NCHAIN];
    char namebuf[32];
    for (uint32_t i = 0; i < NCHAIN; i++) {
        snprintf(namebuf, sizeof(namebuf), "dblk-%05u", i);
        entries[i].name_hash  = lix_hash(namebuf, (uint32_t)strlen(namebuf));
        entries[i].field_slot = lix_slot_of(i * 613u + 17u);
        entries[i].hj_cluster = lix_cluster_of(entries[i].field_slot);
        entries[i].store_off  = (uint64_t)i * 64u;
        entries[i].store_size = 64;
        entries[i].access     = next_u32() % 6u;
        entries[i].flags      = 0;
    }

    static FrustumRouteEvent first[NCHAIN][4];
    int ok_all = 1;
    for (uint32_t i = 0; i < NCHAIN; i++) {
        uint8_t view = (uint8_t)(entries[i].hj_cluster % 8u);
        if (!descend(entries[i].field_slot, view, first[i])) { ok_all = 0; break; }
    }
    check_ok(ok_all, "D0 all 32 descents produce 4 levels");

    /* D1: level order 3,2,1,0 */
    int order_ok = 1;
    for (uint32_t i = 0; i < NCHAIN && order_ok; i++)
        for (int l = 0; l < 4; l++)
            if (first[i][l].level != (uint32_t)(3 - l)) order_ok = 0;
    check_ok(order_ok, "D1 levels visit 3,2,1,0 (coarse->fine)");

    /* D2: capo_key stable = 1<->capo binding */
    int capo_ok = 1;
    for (uint32_t i = 0; i < NCHAIN && capo_ok; i++)
        for (int l = 1; l < 4; l++)
            if (first[i][l].capo_key != first[i][0].capo_key) capo_ok = 0;
    check_ok(capo_ok, "D2 capo_key identical on all 4 levels (1<->capo)");

    /* D3: spans strictly narrow */
    int span_ok = 1;
    for (uint32_t i = 0; i < NCHAIN && span_ok; i++)
        for (int l = 1; l < 4; l++)
            if (!(first[i][l].span_size < first[i][l - 1].span_size)) span_ok = 0;
    check_ok(span_ok, "D3 span_size strictly decreases (refinement)");

    /* D4: branches live in [0,6) */
    int br_ok = 1, seen[6] = {0};
    for (uint32_t i = 0; i < NCHAIN; i++)
        for (int l = 0; l < 4; l++) {
            if (first[i][l].branch_path > 5u) br_ok = 0;
            else seen[first[i][l].branch_path]++;
        }
    int branches_live = 0;
    for (int b = 0; b < 6; b++) if (seen[b]) branches_live++;
    printf("    branches live: %d/6\n", branches_live);
    check_ok(br_ok, "D4 every branch_path in [0,6)");
    check_ok(branches_live == 6, "D4 all 6 directions taken across chains");

    /* D5: replay — identical route_refs */
    int replay_ok = 1;
    for (uint32_t i = 0; i < NCHAIN && replay_ok; i++) {
        FrustumRouteEvent ev[4];
        uint8_t view = (uint8_t)(entries[i].hj_cluster % 8u);
        if (!descend(entries[i].field_slot, view, ev)) { replay_ok = 0; break; }
        for (int l = 0; l < 4; l++)
            if (ev[l].route_ref != first[i][l].route_ref) { replay_ok = 0; break; }
    }
    check_ok(replay_ok, "D5 repeat descent -> identical route_refs (replayable)");

    /* D6: finest event resolves through the memory adapter */
    int ad_ok = 1;
    for (uint32_t i = 0; i < NCHAIN && ad_ok; i++) {
        AdaptiveMemoryRef ref;
        memset(&ref, 0, sizeof(ref));
        if (frustum_route_to_memory_ref(&first[i][3], &ref) != 0) ad_ok = 0;
        if (ref.generation != 0 || ref.span_size == 0) ad_ok = 0;
        if (ref.capo_key != first[i][3].capo_key) ad_ok = 0;
    }
    check_ok(ad_ok, "D6 finest event resolves (generation 0, span kept, capo kept)");

    printf("=== RESULTS: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    printf("Receipt: descent{chains=%d levels=4 capo_stable=%d}\n", NCHAIN, capo_ok);
    return g_fail ? 1 : 0;
}