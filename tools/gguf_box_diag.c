/* gguf_box_diag.c — Diagnose gguf_box offset calculation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gguf_reader.h"
#include "../src/gguf_box.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <model.gguf>\n", argv[0]); return 1; }

    /* Open via gguf_box */
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { fprintf(stderr, "box open fail\n"); return 1; }
    fprintf(stderr, "gguf_box: %u tensors, data_offset=%llu\n",
            box.n_tensors, (unsigned long long)box.data_offset);

    /* Open via gguf_reader for reference */
    GgufReader reader;
    if (gguf_open(argv[1], &reader) != 0) { fprintf(stderr, "reader open fail\n"); return 1; }
    fprintf(stderr, "reader: %u tensors, data_offset=%llu, base=%p\n",
            reader.n_tensors, (unsigned long long)reader.data_offset, (void*)reader.base);

    /* Compare first 5 tensors */
    uint32_t n = box.n_tensors < 5 ? box.n_tensors : 5;
    for (uint32_t i = 0; i < n; i++) {
        const GGUFBoxEntry *be = &box.entries[i];
        fprintf(stderr, "\n[%u] %s\n", i, be->name);
        fprintf(stderr, "  box:  offset=%llu size=%u\n",
                (unsigned long long)be->offset, be->size);
        fprintf(stderr, "  read: offset=%llu size=%u\n",
                (unsigned long long)reader.offsets[i], reader.sizes[i]);

        /* Box pointer should point into mmap'd file at data_offset + offset */
        const uint8_t *expected = reader.base + reader.data_offset + reader.offsets[i];
        const uint8_t *actual = be->data;
        fprintf(stderr, "  expected_ptr=%p actual_ptr=%p\n", (void*)expected, (void*)actual);

        if (expected && actual) {
            int match = (memcmp(expected, actual, 64) == 0);
            fprintf(stderr, "  first_64: %s\n", match ? "MATCH" : "MISMATCH");
            if (!match) {
                fprintf(stderr, "  expected[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        expected[0], expected[1], expected[2], expected[3],
                        expected[4], expected[5], expected[6], expected[7]);
                fprintf(stderr, "  actual[0..7]:   %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        actual[0], actual[1], actual[2], actual[3],
                        actual[4], actual[5], actual[6], actual[7]);
            }
        } else {
            fprintf(stderr, "  NULL pointers\n");
        }
    }

    gguf_close(&reader);
    gguf_box_close(&box);
    return 0;
}
