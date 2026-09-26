/* Run the BFS -> Frustum -> GGUF zero-copy path. */
#include <stdio.h>
#include <stdlib.h>
#include "../core/breathing_fs.h"
#include "../core/gguf_frustum_adapter.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "colab-pack/verify_lora.gguf";
    uint32_t position = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1000u;
    uint8_t view = argc > 3 ? (uint8_t)strtoul(argv[3], NULL, 10) : 2u;
    uint8_t depth = argc > 4 ? (uint8_t)strtoul(argv[4], NULL, 10) : 2u;
    if (position >= 20736u || view >= 8u || depth < 1u || depth > 4u) {
        fprintf(stderr, "usage: %s <gguf> [position<20736] [view<8] [depth 1..4]\n", argv[0]);
        return 2;
    }
    GGUFBox box;
    if (gguf_box_open(&box, path) != 0) {
        fprintf(stderr, "cannot open GGUF: %s\n", path);
        return 1;
    }
    BreathingFS fs;
    bfs_init(&fs);
    fs.seeker.current_pos = position;
    FrustumSeeker seeker = { .position = fs.seeker.current_pos, .view_id = view,
                             .voronoi_mask = 0xFFFFFFu, .frustum_depth = depth };
    FrRouteCache cache;
    FrustumRouteEvent route;
    fr_cache_init(&cache);
    if (!fr_route_produce(&seeker, &cache, &route)) {
        fprintf(stderr, "seeker position is outside Voronoi mask\n");
        gguf_box_close(&box);
        return 1;
    }
    GgufFrustumSpan span;
    int rc = gguf_frustum_resolve(&box, &route, &span);
    if (rc != 0) {
        fprintf(stderr, "GGUF route resolve failed: %d\n", rc);
        gguf_box_close(&box);
        return 1;
    }
    printf("bfs-gguf-frustum: position=%u view=%u depth=%u tensor=%u name=%s offset=%u bytes=%u ptr=%p route_ref=%llu\n",
           position, view, depth, span.tensor_id, span.name, route.span_offset,
           span.size, (const void *)span.data, (unsigned long long)route.route_ref);
    gguf_box_close(&box);
    return 0;
}
