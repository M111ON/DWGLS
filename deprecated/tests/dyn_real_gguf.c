#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "dwgls_dynamic_codec.h"
#include "gguf_reader.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("Usage: %s model.gguf\n", argv[0]); return 1; }

    GgufReader r;
    if (gguf_open(argv[1], &r) != 0) { printf("Failed to open %s\n", argv[1]); return 1; }

    printf("Model: %s (%u tensors)\n", argv[1], r.n_tensors);
    printf("Testing first 20736 weights of each Q8_0 tensor\n\n");

    int found = 0, pass = 0;
    for (uint32_t i = 0; i < r.n_tensors && found < 5; i++) {
        if (r.sizes[i] % 34 == 0 && r.sizes[i] > 34 * 100) {
            uint32_t n_test = 20736;  /* one geometry field */
            uint32_t byte_sz = n_test * 34 / 32;  /* Q8_0 bytes needed */
            if (byte_sz > r.sizes[i]) byte_sz = r.sizes[i];

            uint8_t *buf = (uint8_t *)malloc(r.sizes[i]);
            if (!buf) continue;
            if (gguf_read_tensor(argv[1], &r, i, buf, r.sizes[i]) != 0) {
                free(buf); continue;
            }

            /* Decode Q8_0 to int8 */
            uint32_t n_blocks = byte_sz / 34;
            uint32_t n_weights = n_blocks * 32;
            if (n_weights > n_test) n_weights = n_test;
            int8_t *weights = (int8_t *)malloc(n_weights);
            if (!weights) { free(buf); continue; }

            for (uint32_t b = 0; b < n_blocks && b * 32 < n_weights; b++) {
                uint32_t off = b * 34;
                for (int j = 0; j < 32 && b * 32 + j < n_weights; j++)
                    weights[b * 32 + j] = (int8_t)buf[off + 2 + j];
            }

            DynContainer dc;
            dyn_init(&dc);
            int rc = dyn_encode(&dc, weights, n_weights);
            if (rc == 0) {
                float ratio = dyn_ratio(&dc);
                int vr = dyn_verify(weights, n_weights, &dc);
                printf("[%u] \"%s\" (%u w) → %s ratio=%.4f %s\n",
                       i, r.names[i], n_weights,
                       dyn_strategy_name(dc.header.strategy), ratio,
                       vr == 0 ? "LOSSLESS" : "MISMATCH");
                if (vr == 0) pass++;
            }
            free(weights);
            free(buf);
            found++;
        }
    }
    gguf_close(&r);
    printf("\n%d/%d LOSSLESS\n", pass, found);
    return 0;
}
