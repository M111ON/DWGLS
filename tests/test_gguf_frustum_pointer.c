/* GGUFBox named zero-copy pointer through a Frustum route. */
#include <stdio.h>
#include <string.h>
#include "../core/gguf_frustum_adapter.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "colab-pack/verify_lora.gguf";
    GGUFBox box;
    if (gguf_box_open(&box, path) != 0) {
        printf("SKIP: cannot open %s\n", path);
        return 0;
    }

    FrustumRouteEvent route = {0};
    route.tensor_id = 6;
    route.span_offset = 0;
    route.span_size = box.entries[route.tensor_id].size;
    GgufFrustumSpan span;
    int rc = gguf_frustum_resolve(&box, &route, &span);
    int pass = rc == 0 && span.data == box.entries[6].data &&
               span.size == box.entries[6].size && span.name != NULL;
    printf("GGUF Frustum pointer: tensor=%u name=%s bytes=%u %s\n",
           span.tensor_id, pass ? span.name : "-", span.size,
           pass ? "PASS" : "FAIL");
    gguf_box_close(&box);
    return pass ? 0 : 1;
}
