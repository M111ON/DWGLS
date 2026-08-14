/* show_weights.c — Dump first Q8_0 block weights with sign analysis */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gguf_reader.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "model/smolVLM-256M-Instruct-text.Q8_0.gguf";
    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("Cannot open %s\n", path); return 1; }

    /* Find first Q8_0 tensor */
    int idx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) { idx = (int)i; break; }
    }
    if (idx < 0) { printf("No Q8_0 tensor found\n"); gguf_close(gf); return 1; }

    GGUF_Tensor *t = &gf->tensors[idx];
    printf("Tensor: %s (%llu weights, %llu blocks)\n",
        t->name, (unsigned long long)t->n_weights, (unsigned long long)(t->n_weights/32));

    /* Seek to tensor data */
    uint64_t data_start = gf->tensor_data_start + t->offset;
    data_start = (data_start + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)data_start, SEEK_SET);

    /* Read first 5 blocks */
    for (int b = 0; b < 5; b++) {
        uint16_t scale;
        int8_t w[32];
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;

        printf("\n=== Block %d (scale=0x%04x) ===\n", b, scale);
        printf("  weights: ");
        for (int i = 0; i < 32; i++) printf("%4d", w[i]);
        printf("\n");

        /* Sign analysis */
        int match = 0, mismatch = 0;
        printf("  sign:    ");
        for (int i = 0; i < 32; i++) {
            char actual = (w[i] >= 0) ? '+' : '-';
            char expected = (i % 2 == 0) ? '+' : '-';
            if (actual == expected) match++; else mismatch++;
            printf("   %c", actual);
        }
        printf("\n  expected: ");
        for (int i = 0; i < 32; i++) printf("   %c", (i%2==0)?'+':'-');
        printf("\n  match=%d mismatch=%d\n", match, mismatch);
    }

    /* Overall statistics */
    printf("\n═══ Overall sign pattern analysis (first 1000 blocks) ═══\n");
    fseek(gf->fp, (long)data_start, SEEK_SET);
    uint64_t total_match = 0, total_mismatch = 0;
    uint64_t even_pos = 0, even_neg = 0, odd_pos = 0, odd_neg = 0;
    int8_t w[32];
    for (int b = 0; b < 1000; b++) {
        uint16_t sc;
        if (fread(&sc, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;
        for (int i = 0; i < 32; i++) {
            int is_even = (i % 2 == 0);
            int is_pos = (w[i] >= 0);
            if (is_even && is_pos) even_pos++;
            else if (is_even && !is_pos) even_neg++;
            else if (!is_even && is_pos) odd_pos++;
            else odd_neg++;
            if (is_even == is_pos) total_match++; else total_mismatch++;
        }
    }
    uint64_t total = total_match + total_mismatch;
    printf("  Even index + Positive: %llu\n", (unsigned long long)even_pos);
    printf("  Even index + Negative: %llu\n", (unsigned long long)even_neg);
    printf("  Odd  index + Positive: %llu\n", (unsigned long long)odd_pos);
    printf("  Odd  index + Negative: %llu\n", (unsigned long long)odd_neg);
    printf("  match (even=+/odd=-):  %llu / %llu (%.1f%%)\n",
        (unsigned long long)total_match, (unsigned long long)total,
        100.0 * total_match / total);

    gguf_close(gf);
    return 0;
}
