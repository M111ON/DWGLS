/* tools/bfs_tensor_pipeline.c — BreathingFS tensor pipeline consumer
 *
 * Wires: BreathingFS seeker → Frustum Route Producer → TensorStore (tesspack)
 *
 * Input: .tesspack (model), seeker position, view_id, voronoi_mask, frustum_depth
 * Output: AdaptiveRouteEvent stream + tensor prefetch + RSS measurement
 *
 * BUILD: gcc -O2 -Icore -o build/bfs_tensor_pipeline tools/bfs_tensor_pipeline.c
 * RUN:   ./build/bfs_tensor_pipeline <tesspack> [position] [view_id] [depth]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <windows.h>
#include <psapi.h>
#include "../core/breathing_fs.h"
#include "../core/frustum_route.h"
#include "../core/geo_tess_container.h"
#include "../core/geo_voronoi_mask.h"

static double rss_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc = {0};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s <tesspack> [position] [view_id] [frustum_depth]\n", prog);
    printf("  position:    seeker flat position [0,20736) default=0\n");
    printf("  view_id:     frustum view [0,7] default=0\n");
    printf("  frustum_depth: levels [1,4] default=2\n");
    printf("\nProduces route events for tensors in seeker window, prefetches from tesspack.\n");
}

static int prefetch_tensor_range(TESS_PackIndex *pi, const char *name,
                                 uint64_t span_offset, uint32_t span_size) {
    if (span_size == 0) return 0;
    TESS_CapoReader cr;
    int capo_id = (int)(span_offset / TESS_TOTAL_SLOTS);
    uint32_t slot_offset = (uint32_t)(span_offset % TESS_TOTAL_SLOTS);
    if (tess_pack_get_capo(pi, &cr, name, capo_id) != 0) return -1;
    uint8_t *dummy = (uint8_t *)malloc(span_size);
    if (!dummy) return -2;
    uint32_t got = tess_capo_load_range(&cr, slot_offset, span_size, dummy);
    free(dummy);
    return (got == span_size) ? 0 : -3;
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char *tesspack_path = argv[1];
    uint32_t position = argc > 2 ? (uint32_t)atoi(argv[2]) : 0;
    uint32_t view_id = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;
    uint32_t frustum_depth = argc > 4 ? (uint32_t)atoi(argv[4]) : 2;

    if (position >= 20736) { fprintf(stderr, "position must be < 20736\n"); return 1; }
    if (view_id >= 8) { fprintf(stderr, "view_id must be < 8\n"); return 1; }
    if (frustum_depth < 1 || frustum_depth > 4) { fprintf(stderr, "frustum_depth must be 1-4\n"); return 1; }

    printf("=== BreathingFS Tensor Pipeline ===\n");
    printf("tesspack: %s\n", tesspack_path);
    printf("seeker pos: %u, view: %u, depth: %u\n", position, view_id, frustum_depth);
    printf("RSS before: %.2f MB\n", rss_mb());

    /* Load tesspack index */
    TESS_PackIndex *pi = tess_pack_open(tesspack_path);
    if (!pi) { fprintf(stderr, "Failed to open tesspack: %s\n", tesspack_path); return 1; }
    printf("Loaded tesspack: %u entries, %u capos\n", pi->n_entries, pi->n_capos);

    /* Initialize BreathingFS seeker */
    BreathingFS fs;
    bfs_init(&fs);
    fs.seeker.current_pos = position;
    fs.seeker.scale = 1.0;
    fs.seeker.window = BFS_WINDOW;
    fs.seeker.space = BFS_TOTAL_SLOTS;

    /* Build voronoi mask from seeker position (24 cells, 1 bit each) */
    uint32_t voronoi_mask = 0;
    uint32_t cell = vm_cell_of(position);
    voronoi_mask |= (1u << cell);
    for (int d = 1; d <= 2; d++) {
        int c = (int)cell - d;
        if (c >= 0) voronoi_mask |= (1u << c);
        c = (int)cell + d;
        if (c < VM_CELLS) voronoi_mask |= (1u << c);
    }

    /* Create frustum seeker */
    FrustumSeeker fseeker = {
        .position = position,
        .view_id = view_id,
        .voronoi_mask = voronoi_mask,
        .frustum_depth = frustum_depth
    };

    /* Initialize route cache */
    FrRouteCache cache;
    fr_route_cache_init(&cache);

    /* Route tensor names from tesspack */
    printf("\n=== Route Events ===\n");
    int events_found = 0;
    for (uint32_t i = 0; i < pi->n_entries; i++) {
        TESS_PackEntry *e = &pi->entries[i];
        AdaptiveRouteEvent ev;
        int rc = fr_route_tensor(&fseeker, &cache, e->name, e->capo_id, e->span_offset, e->span_size, &ev);
        if (rc == 0) {
            printf("  [%d] L%d branch=%u view=%u node=%u capo=%u tid=%u off=%zu sz=%u ref=%u\n",
                   events_found, ev.level, ev.branch_path, ev.view, ev.node_id,
                   ev.capo_key, ev.tensor_id, ev.span_offset, ev.span_size, ev.route_ref);
            /* Prefetch tensor data from tesspack */
            int prefetch_rc = prefetch_tensor_range(pi, e->name, ev.span_offset, ev.span_size);
            if (prefetch_rc == 0) {
                printf("       -> prefetched OK\n");
            } else {
                printf("       -> prefetch FAILED (%d)\n", prefetch_rc);
            }
            events_found++;
        }
    }

    printf("\nRSS after prefetch: %.2f MB\n", rss_mb());
    printf("Events produced: %d / %u entries\n", events_found, pi->n_entries);
    printf("Route cache size: %zu\n", cache.count);

    /* Test idempotency: route again, should get same refs */
    printf("\n=== Idempotency Test ===\n");
    int idempotent = 0;
    for (uint32_t i = 0; i < pi->n_entries; i++) {
        TESS_PackEntry *e = &pi->entries[i];
        AdaptiveRouteEvent ev1, ev2;
        int rc1 = fr_route_tensor(&fseeker, &cache, e->name, e->capo_id, e->span_offset, e->span_size, &ev1);
        int rc2 = fr_route_tensor(&fseeker, &cache, e->name, e->capo_id, e->span_offset, e->span_size, &ev2);
        if (rc1 == 0 && rc2 == 0 && ev1.route_ref == ev2.route_ref) idempotent++;
    }
    printf("Idempotent routes: %d / %u\n", idempotent, pi->n_entries);

    /* Test multiple seekers merging masks */
    printf("\n=== Multi-Seeker Mask Merge ===\n");
    FrustumSeeker seeker2 = fseeker;
    seeker2.position = (position + 5184) % 20736;
    uint32_t merged_mask = fseeker.voronoi_mask | seeker2.voronoi_mask;
    printf("Seeker1 pos=%u mask=0x%06X\n", fseeker.position, fseeker.voronoi_mask);
    printf("Seeker2 pos=%u mask=0x%06X\n", seeker2.position, seeker2.voronoi_mask);
    printf("Merged mask=0x%06X (popcount=%d)\n", merged_mask, __builtin_popcount(merged_mask));

    /* Scale seeker and re-route */
    printf("\n=== Scale Change Test ===\n");
    double old_scale = fs.seeker.scale;
    bfs_move_seeker(&fs, fs.seeker.current_pos, fs.seeker.scale * 2.0);
    printf("Scale: %.3f -> %.3f, window: %u -> %u\n", old_scale, fs.seeker.scale, BFS_WINDOW, fs.seeker.window);

    fseeker.position = fs.seeker.current_pos;
    fseeker.frustum_depth = 2;
    FrRouteCache cache2;
    fr_route_cache_init(&cache2);
    int scaled_events = 0;
    for (uint32_t i = 0; i < pi->n_entries; i++) {
        TESS_PackEntry *e = &pi->entries[i];
        AdaptiveRouteEvent ev;
        if (fr_route_tensor(&fseeker, &cache2, e->name, e->capo_id, e->span_offset, e->span_size, &ev) == 0) {
            scaled_events++;
        }
    }
    printf("Events at scale %.3f: %d\n", fs.seeker.scale, scaled_events);

    tess_pack_close(pi);
    printf("\n=== Pipeline Complete ===\n");
    return 0;
}