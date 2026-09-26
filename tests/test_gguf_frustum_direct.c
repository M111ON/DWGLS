/* GGUF -> Frustum -> direct mmap span proof. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_reader.h"
#include "../core/frustum_route.h"
#include "../core/frustum_memory_adapter.h"

typedef struct { GgufReader *reader; } Catalog;

static int resolve_gguf(const FrustumRouteEvent *route,
                        AdaptiveMemoryRef *out, void *opaque) {
    Catalog *cat = (Catalog *)opaque;
    if (!route || !out || !cat || !cat->reader || !cat->reader->n_tensors) return -1;
    uint32_t i = route->tensor_id % cat->reader->n_tensors;
    uint64_t off = cat->reader->data_offset + cat->reader->offsets[i];
    uint32_t size = cat->reader->sizes[i];
    if (off > cat->reader->base_sz || size > cat->reader->base_sz - off) return -1;
    out->node_id = route->node_id;
    out->capo_key = route->capo_key;
    out->tensor_id = i;
    out->span_offset = 0;
    out->span_size = size;
    out->access_kind = 1;
    out->generation = route->level;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "colab-pack/verify_lora.gguf";
    GgufReader reader;
    if (gguf_open(path, &reader) != 0) {
        printf("SKIP: cannot open %s\n", path);
        return 0;
    }
    Catalog cat = { &reader };
    FrustumSeeker seeker = { .position = 1000, .view_id = 2,
                             .voronoi_mask = 0xFFFFFFu, .frustum_depth = 2 };
    FrRouteCache cache;
    FrustumRouteEvent route;
    fr_cache_init(&cache);
    if (!fr_route_produce(&seeker, &cache, &route)) return 1;

    AdaptiveRouteEvent event;
    if (frustum_route_to_resolved_event(&route, resolve_gguf, &cat, &event) != 0)
        return 2;
    const uint8_t *mapped = reader.base + reader.data_offset + reader.offsets[event.memory.tensor_id];
    uint8_t *copy = (uint8_t *)malloc(event.memory.span_size);
    if (!copy) return 3;
    int rc = gguf_read_tensor(path, &reader, event.memory.tensor_id,
                              copy, event.memory.span_size);
    int same = rc == 0 && memcmp(mapped, copy, event.memory.span_size) == 0;
    printf("GGUF direct frustum: tensors=%u tensor=%u bytes=%u %s\n",
           reader.n_tensors, event.memory.tensor_id, event.memory.span_size,
           same ? "PASS" : "FAIL");
    free(copy);
    gguf_close(&reader);
    return same ? 0 : 4;
}
