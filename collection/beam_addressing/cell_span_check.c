/*
 * Quick diagnostic: cell distribution within blocks of 32 weights
 * Compile: gcc -O2 -I. cell_span_check.c -o cell_span_check.exe -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "gguf_reader.h"
#include "beam_geometric.c"

int main(void) {
    GGUF_File *gf = gguf_open("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf");
    if (!gf) { fprintf(stderr,"FAIL open\n"); return 1; }
    int ti = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (gf->tensors[i].type == 8) { ti = (int)i; break; }
    if (ti < 0) { fprintf(stderr,"FAIL no Q8\n"); return 1; }
    GGUF_Tensor *t = &gf->tensors[ti];
    uint64_t data_start = gf->tensor_data_start + t->offset;
    data_start = (data_start + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)data_start, SEEK_SET);

    int32_t ds[] = {FP_SCALE*1, FP_SCALE*2, FP_SCALE*4, FP_SCALE*8,
                    FP_SCALE*16, FP_SCALE*32, FP_SCALE*64, FP_SCALE*128};

    for (int d = 0; d < 8; d++) {
        GeoChain ch = geo_chain_init(ds[d], 65536);
        rewind(gf->fp);
        data_start = gf->tensor_data_start + t->offset;
        data_start = (data_start + 31) & ~(uint64_t)31;
        fseek(gf->fp, (long)data_start, SEEK_SET);

        int8_t buf[32];
        int max_span = 0;
        int blocks_with_span_gt_15 = 0;
        int total_blocks = 0;
        int total_cell_span = 0;

        for (int b = 0; b < 200000 && b < (int)(t->n_weights/32); b++) {
            uint16_t sc;
            if (fread(&sc,2,1,gf->fp)!=1) break;
            if (fread(buf,1,32,gf->fp)!=32) break;
            int block_min = 9999, block_max = -1;
            for (int i = 0; i < 32; i++) {
                int32_t w_q12 = (int32_t)buf[i] * FP_SCALE;
                int32_t packed = geo_chain_encode(&ch, w_q12);
                uint32_t ap = (uint32_t)((packed < 0) ? -packed : packed);
                int cell = (int)((ap >> 16) & 0xFFFF);
                if (cell < block_min) block_min = cell;
                if (cell > block_max) block_max = cell;
            }
            int span = block_max - block_min;
            total_cell_span += span;
            if (span > max_span) max_span = span;
            if (span > 15) blocks_with_span_gt_15++;
            total_blocks++;
        }
        printf("D=%-7.2f | max_span=%d  avg_span=%.1f  blocks_span>15=%d/%d  cell_bits=%d\n",
               (double)ds[d]/FP_SCALE, max_span,
               (double)total_cell_span/total_blocks,
               blocks_with_span_gt_15, total_blocks,
               max_span > 0 ? (int)(log2(max_span)+1) : 1);
    }
    gguf_close(gf);
    return 0;
}
