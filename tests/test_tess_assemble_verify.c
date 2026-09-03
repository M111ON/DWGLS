#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gguf_reader.h"

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "Usage: %s <original.gguf> <assembled.gguf>\n", argv[0]); return 1; }
    
    GgufReader r1 = {0}, r2 = {0};
    if (gguf_open(argv[1], &r1) != 0) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }
    if (gguf_open(argv[2], &r2) != 0) { fprintf(stderr, "Cannot open %s\n", argv[2]); gguf_close(&r1); return 1; }
    
    fprintf(stderr, "Original: %u tensors, Assembled: %u tensors\n", r1.n_tensors, r2.n_tensors);
    
    int match = 0, mismatch = 0, skip = 0;
    uint32_t n = r1.n_tensors < r2.n_tensors ? r1.n_tensors : r2.n_tensors;
    for (uint32_t i = 0; i < n && i < 10; i++) {
        uint32_t s1 = r1.sizes[i], s2 = r2.sizes[i];
        if (s1 != s2) { fprintf(stderr, "  [%s] SIZE MISMATCH: %u vs %u\n", r1.names[i], s1, s2); mismatch++; continue; }
        if (s1 > 65536) { fprintf(stderr, "  [%s] SKIP (too large: %u)\n", r1.names[i], s1); skip++; continue; }
        uint8_t *d1 = (uint8_t*)malloc(s1);
        uint8_t *d2 = (uint8_t*)malloc(s2);
        if (gguf_read_tensor(argv[1], &r1, i, d1, s1) != 0 ||
            gguf_read_tensor(argv[2], &r2, i, d2, s2) != 0) {
            fprintf(stderr, "  [%s] READ FAIL\n", r1.names[i]); mismatch++;
        } else if (memcmp(d1, d2, s1) == 0) {
            fprintf(stderr, "  [%s] MATCH (%u bytes)\n", r1.names[i], s1); match++;
        } else {
            fprintf(stderr, "  [%s] DATA MISMATCH (%u bytes)\n", r1.names[i], s1); mismatch++;
        }
        free(d1); free(d2);
    }
    
    fprintf(stderr, "Result: %d match, %d mismatch, %d skipped\n", match, mismatch, skip);
    gguf_close(&r1); gguf_close(&r2);
    return mismatch > 0 ? 1 : 0;
}
