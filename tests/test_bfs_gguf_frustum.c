/* BreathingFS seeker -> Frustum -> GGUF zero-copy span proof. */
#include <stdio.h>
#include <stdint.h>
#include "../core/breathing_fs.h"
#include "../core/gguf_frustum_adapter.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "colab-pack/verify_lora.gguf";
    GGUFBox box;
    if (gguf_box_open(&box, path) != 0) {
        printf("SKIP: cannot open %s\n", path);
        return 0;
    }

    BreathingFS fs;
    bfs_init(&fs);
    fs.seeker.current_pos = 1000;
    fs.seeker.scale = 1.0;

    FrustumSeeker seeker = {
        .position = fs.seeker.current_pos,
        .view_id = 2,
        .voronoi_mask = 0xFFFFFFu,
        .frustum_depth = 2
    };
    FrRouteCache cache;
    FrustumRouteEvent route;
    fr_cache_init(&cache);
    if (!fr_route_produce(&seeker, &cache, &route)) {
        gguf_box_close(&box);
        return 1;
    }

    GgufFrustumSpan span;
    int rc = gguf_frustum_resolve(&box, &route, &span);
    const GGUFBoxEntry *entry = &box.entries[route.tensor_id];
    int pass = rc == 0 && span.data == entry->data + route.span_offset &&
               span.size == route.span_size;
    printf("BFS GGUF Frustum: pos=%u view=%u tensor=%u name=%s bytes=%u rc=%d %s\n",
           seeker.position, seeker.view_id, span.tensor_id,
           pass ? span.name : entry->name, span.size, rc,
           pass ? "PASS" : "FAIL");
    gguf_box_close(&box);
    return pass ? 0 : 2;
}
