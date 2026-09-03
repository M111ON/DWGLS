#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "geo_tess_container.h"

static const uint32_t CELL_SIZES[] = {4,2,18,20,0,0,22,24,34,36,84,110,144,176,210,292};
#define NCELLS (sizeof(CELL_SIZES)/sizeof(CELL_SIZES[0]))

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: tess_perf <model.gguf>\n"); return 1; }

    uint64_t total_cells = 0;
    uint64_t total_bytes = 0;

    for (int d = 0; d < (int)NCELLS; d++) {
        if (CELL_SIZES[d] == 0) continue;
        uint32_t csz = CELL_SIZES[d];
        for (int trial = 0; trial < 3; trial++) {
            uint8_t *src = (uint8_t *)malloc((uint64_t)TESS_TOTAL_SLOTS * csz);
            uint8_t *cube = (uint8_t *)malloc((uint64_t)TESS_TOTAL_SLOTS * csz);
            uint8_t *out  = (uint8_t *)malloc((uint64_t)TESS_TOTAL_SLOTS * csz);
            for (uint64_t i = 0; i < (uint64_t)TESS_TOTAL_SLOTS * csz; i++) src[i] = (uint8_t)(i & 0xFF);

            clock_t t0 = clock();
            for (uint32_t i = 0; i < TESS_TOTAL_SLOTS; i++) {
                uint32_t slot = tess_stride_scatter(i);
                memcpy(cube + (uint64_t)slot * csz, src + (uint64_t)i * csz, csz);
            }
            clock_t t1 = clock();
            for (uint32_t i = 0; i < TESS_TOTAL_SLOTS; i++) {
                uint32_t slot = tess_stride_scatter(i);
                memcpy(out + (uint64_t)i * csz, cube + (uint64_t)slot * csz, csz);
            }
            clock_t t2 = clock();
            int ok = memcmp(src, out, (uint64_t)TESS_TOTAL_SLOTS * csz) == 0;

            double enc_ms = (double)(t1-t0)/CLOCKS_PER_SEC*1000;
            double dec_ms = (double)(t2-t1)/CLOCKS_PER_SEC*1000;
            uint64_t bytes = (uint64_t)TESS_TOTAL_SLOTS * csz;
            double gbps_enc = (double)bytes/1e9 / (enc_ms/1000);
            double gbps_dec = (double)bytes/1e9 / (dec_ms/1000);
            printf("cell=%3u  enc=%.2fms  dec=%.2fms  %.1fGB/s enc  %.1fGB/s dec  %s\n",
                   csz, enc_ms, dec_ms, gbps_enc, gbps_dec, ok?"OK":"FAIL");

            total_cells += TESS_TOTAL_SLOTS;
            total_bytes += bytes;
            free(src); free(cube); free(out);
        }
    }
    printf("\nTotal: %lu cells, %.1f MB scattered\n", (unsigned long)total_cells, (double)total_bytes/1e6);
    return 0;
}
