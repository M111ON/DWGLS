/* tesspack_verify: verify .tesspack against original GGUF — lossless check.
 * Usage: tesspack_verify <model.gguf> <model.tesspack> [tensor_filter]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <model.gguf> <model.tesspack> [tensor_filter]\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *pack_path = argv[2];
    const char *filter = (argc > 3) ? argv[3] : NULL;

    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        fprintf(stderr, "Cannot open GGUF: %s\n", gguf_path);
        return 1;
    }

    uint32_t total = 0, matched = 0, failed = 0, skipped = 0;

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (filter && !strstr(gguf.names[i], filter)) continue;
        total++;

        uint32_t dtype = gguf.dtypes[i];
        uint32_t csz = (dtype < 16) ? GGUF_CELL_SIZE[dtype] : 0;
        if (csz == 0) { skipped++; continue; }

        uint32_t n_blocks = gguf.sizes[i] / csz;
        if (n_blocks == 0) { skipped++; continue; }
        uint32_t n_capos = (n_blocks + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;

        const uint8_t *tensor_data = gguf.base + gguf.data_offset + gguf.offsets[i];

        for (uint32_t c = 0; c < n_capos; c++) {
            uint32_t off = c * TESS_TOTAL_SLOTS;
            uint32_t chunk = n_blocks - off;
            if (chunk > TESS_TOTAL_SLOTS) chunk = TESS_TOTAL_SLOTS;

            TESS_CapoReader cr;
            if (tess_capo_open_pack(&cr, pack_path, gguf.names[i], c) != 0) {
                fprintf(stderr, "FAIL open pack: %s capo %u\n", gguf.names[i], c);
                failed++;
                continue;
            }

            uint8_t *decoded = (uint8_t *)malloc((uint64_t)chunk * csz);
            tess_capo_load_range(&cr, 0, chunk, decoded);
            tess_capo_close(&cr);

            const uint8_t *expected = tensor_data + (uint64_t)off * csz;
            if (memcmp(decoded, expected, (uint64_t)chunk * csz) == 0) {
                matched++;
            } else {
                fprintf(stderr, "MISMATCH: %s capo %u\n", gguf.names[i], c);
                for (uint32_t j = 0; j < chunk * csz; j++) {
                    if (decoded[j] != expected[j]) {
                        fprintf(stderr, "  first diff at cell %u byte %u: got %02x expected %02x\n",
                                j / csz, j % csz, decoded[j], expected[j]);
                        break;
                    }
                }
                failed++;
            }
            free(decoded);
        }
    }

    printf("Verify: %u/%u capos matched, %u failed, %u skipped\n", matched, total, failed, skipped);
    gguf_close(&gguf);
    return failed ? 1 : 0;
}
