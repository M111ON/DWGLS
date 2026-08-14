/*
 * Quick diagnostic: print RDH addresses for all tensors, flag invalid ones.
 * Compile: gcc -O2 -std=c11 -I. -I../../runner -I../../collection -I../../collection/Hfolder -I../../collection/rdh -o rdh_diag.exe rdh_diag.c ../../runner/gguf_index.c -lm
 * But gguf_index.c doesn't exist. Let me just inline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rdh_addr.h"
#include "../../runner/addr_space.h"
#include "../../runner/gguf_index.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: rdh_diag.exe model.gguf\n"); return 1; }

    GGUFTensorIndex idx;
    memset(&idx, 0, sizeof(idx));
    if (gguf_idx_open(argv[1], &idx) != 0) { fprintf(stderr, "ERROR: can't open\n"); return 1; }

    int invalid = 0;
    uint32_t used[20736] = {0};
    int collisions = 0;
    int dropped_non_block = 0, dropped_block = 0;

    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        uint32_t rdh = addr_from_rdh_name(idx.names[i], 0);
        uint32_t hash = addr_from_tensor_name(idx.names[i], 0);
        if (rdh >= ADDR_BASE) {
            invalid++;
            int is_block = (idx.names[i][0]=='b'&&idx.names[i][1]=='l'&&idx.names[i][2]=='k'&&idx.names[i][3]=='.');
            printf("  DROPPED [%s]: addr=%u ring=%u wedge=%u hash=%u\n",
                   idx.names[i], rdh,
                   (uint32_t)(rdh / 256), (uint32_t)(rdh % 256), hash);
            if (is_block) dropped_block++; else dropped_non_block++;
        }
        if (rdh < 20736) {
            if (used[rdh]) { collisions++; printf("  COLLISION: addr=%u \"%s\"\n", rdh, idx.names[i]); }
            else used[rdh]=1;
        }
    }

    printf("\nTotal: %llu tensors\n", (unsigned long long)idx.n_tensors);
    printf("Dropped: %d (block=%d non-block=%d)\n", invalid, dropped_block, dropped_non_block);
    printf("Collisions: %d\n", collisions);
    printf("Valid: %llu\n", (unsigned long long)idx.n_tensors - invalid);
    gguf_idx_close(&idx);
    return 0;
}
