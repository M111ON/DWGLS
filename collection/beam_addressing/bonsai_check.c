/* Quick Bonsai Q1_0 analysis */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "gguf_reader.h"

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "F:/model/Bonsai-27B-Q1_0.gguf";
    
    printf("=== Bonsai Q1_0 Analysis ===\n\n");
    
    GGUF_File *gf = gguf_open(path);
    if (!gf) { fprintf(stderr, "Cannot open %s\n", path); return 1; }
    
    printf("Version: %u  Tensors: %llu  KV pairs: %llu\n",
        gf->version, (unsigned long long)gf->tensor_count, (unsigned long long)gf->kv_count);
    printf("Data offset: %llu bytes\n\n", (unsigned long long)gf->tensor_data_start);
    
    for (uint64_t i = 0; i < gf->tensor_count && i < 15; i++) {
        GGUF_Tensor *ti = &gf->tensors[i];
        uint64_t ne = 1;
        for (uint32_t d = 0; d < ti->n_dims; d++) ne *= ti->dims[d];
        
        printf("  [%llu] %-55s  type=%u  [",
            (unsigned long long)i, ti->name, ti->type);
        for (uint32_t d = 0; d < ti->n_dims; d++) {
            if (d > 0) printf("x");
            printf("%llu", (unsigned long long)ti->dims[d]);
        }
        printf("]  ne=%llu\n", (unsigned long long)ne);
    }
    
    gguf_close(gf);
    return 0;
}
