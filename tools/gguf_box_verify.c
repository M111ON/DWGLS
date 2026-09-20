/* gguf_box_verify.c — Verify gguf_box returns correct tensor data.
 * Compares box pointer data vs raw file read at correct offset (data_offset + tensor_offset). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gguf_reader.h"
#include "../src/gguf_box.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <model.gguf>\n", argv[0]); return 1; }

    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { fprintf(stderr, "box open fail\n"); return 1; }
    fprintf(stderr, "gguf_box: %u tensors, data_offset=%llu\n", box.n_tensors, (unsigned long long)box.data_offset);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "file open fail\n"); gguf_box_close(&box); return 1; }

    int checked = 0, matched = 0;
    for (unsigned i = 0; i < box.n_tensors; i++) {
        const GGUFBoxEntry *e = &box.entries[i];
        if (e->size == 0 || !e->data) continue;

        size_t nbytes = e->size;
        /* Seek to data_offset + tensor_offset (absolute file position) */
        size_t file_pos = (size_t)box.data_offset + (size_t)e->offset;
        uint8_t *file_data = (uint8_t *)malloc(nbytes);
        fseek(f, (long)file_pos, SEEK_SET);
        size_t rd = fread(file_data, 1, nbytes, f);

        checked++;
        if (rd == nbytes && memcmp(e->data, file_data, nbytes) == 0) {
            matched++;
        } else {
            fprintf(stderr, "MISMATCH: %s (read=%zu vs %zu bytes)\n", e->name, rd, nbytes);
        }
        free(file_data);
        if (checked >= 10) break; /* check first 10 tensors */
    }

    fprintf(stderr, "RESULT: %d/%d tensors MATCH\n", matched, checked);
    fclose(f);
    gguf_box_close(&box);
    return (checked > 0 && matched == checked) ? 0 : 1;
}
