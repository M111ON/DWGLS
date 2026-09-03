/* Self-contained .tesspack roundtrip: create capos in memory → pack → open from pack → verify */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/geo_tess_container.h"

static uint32_t xs32(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}

int main(void) {
    int pass = 0, fail = 0;

    printf("=== .tesspack roundtrip test ===\n");

    struct { uint32_t csz; uint32_t ne; const char *name; } cases[] = {
        { 4,  20736, "layer.0.weight" },
        { 144, 10000, "layer.1.ffn_down" },
        { 2,  500,   "layer.2.embed" },
    };
    int NC = 3;

    /* Phase 1: create synthetic capo buffers + save reference decoded data */
    uint8_t *capo_bufs[3];
    uint32_t capo_sizes[3];
    uint8_t *refs[3];

    for (int c = 0; c < NC; c++) {
        uint32_t csz = cases[c].csz, ne = cases[c].ne;
        uint32_t cube_bytes = TESS_TOTAL_SLOTS * csz;
        uint32_t total_sz = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;
        uint8_t *buf = (uint8_t *)calloc(1, total_sz);

        uint32_t seed = 0xDEAD0001u + (uint32_t)c;
        uint8_t *cube = buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
        for (uint32_t i = 0; i < ne; i++) {
            uint32_t slot = tess_stride_scatter(i);
            if (slot >= TESS_TOTAL_SLOTS) slot = i % TESS_TOTAL_SLOTS;
            uint8_t *dst = cube + (uint64_t)slot * csz;
            for (uint32_t b = 0; b < csz; b++)
                dst[b] = (uint8_t)(xs32(&seed) & 0xFF);
        }

        TESS_Header *h = (TESS_Header *)buf;
        tess_header_init(h, 0, csz);
        h->tensor_count = ne;
        h->x_slots = TESS_X_SLOTS; h->y_slots = TESS_Y_SLOTS; h->z_slots = TESS_Z_SLOTS;

        TESS_Formula *f = (TESS_Formula *)(buf + TESS_HEADER_SIZE);
        tess_formula_init(f);
        f->mirror_axis_x = h->x_slots; f->mirror_axis_y = h->y_slots; f->mirror_axis_z = h->z_slots;

        uint64_t crc = tess_crc64(cube, cube_bytes);
        memcpy(cube + cube_bytes, &crc, TESS_CRC_SIZE);

        capo_bufs[c] = buf;
        capo_sizes[c] = total_sz;

        /* decode reference */
        TESS_CapoReader r;
        tess_capo_open_buf(&r, buf, total_sz);
        refs[c] = (uint8_t *)malloc((size_t)ne * csz);
        tess_capo_load_range(&r, 0, ne, refs[c]);
        tess_capo_close(&r);

        printf("  capo %d: %s (%u cells, %u B/cell)\n", c, cases[c].name, ne, csz);
    }

    /* Phase 2: write .tesspack file from memory (no directory scan) */
    printf("\nWriting .tesspack...\n");
    {
        FILE *fp = fopen("build/test_rt.tesspack", "wb");
        if (!fp) { fprintf(stderr, "FAIL: cannot create pack file\n"); return 1; }
        fseek(fp, 64, SEEK_SET);  /* skip header */

        uint64_t offsets[3];
        for (int c = 0; c < NC; c++) {
            offsets[c] = (uint64_t)ftell(fp);
            fwrite(capo_bufs[c], 1, capo_sizes[c], fp);
        }

        uint32_t index_offset = (uint32_t)ftell(fp);

        /* write index entries */
        for (int c = 0; c < NC; c++) {
            uint8_t nlen = (uint8_t)strlen(cases[c].name);
            fwrite(&nlen, 1, 1, fp);
            fwrite(cases[c].name, 1, nlen, fp);
            uint32_t capo_id = 0;
            fwrite(&capo_id, 4, 1, fp);
            fwrite(&offsets[c], 8, 1, fp);
            fwrite(&capo_sizes[c], 4, 1, fp);
        }

        /* write header */
        rewind(fp);
        uint32_t hdr[16] = {0};
        hdr[0] = 0x5450414Bu; /* TPAK */
        hdr[1] = 1;           /* version */
        hdr[2] = (uint32_t)NC; /* n_capos */
        hdr[3] = index_offset;
        fwrite(hdr, 1, 64, fp);
        fclose(fp);
        printf("  wrote test_rt.tesspack (%d capos)\n", NC);
    }

    /* Phase 3: verify each capo from pack */
    printf("\nVerifying from pack...\n");
    for (int c = 0; c < NC; c++) {
        uint32_t ne = cases[c].ne, csz = cases[c].csz;

        TESS_CapoReader r;
        int rc = tess_capo_open_pack(&r, "build/test_rt.tesspack", cases[c].name, 0);
        if (rc != 0) {
            printf("  FAIL: pack open %s rc=%d\n", cases[c].name, rc);
            fail++; continue;
        }

        uint8_t *got = (uint8_t *)malloc((size_t)ne * csz);
        tess_capo_load_range(&r, 0, ne, got);
        tess_capo_close(&r);

        if (memcmp(got, refs[c], (size_t)ne * csz) == 0) {
            printf("  PASS: %s (%u cells)\n", cases[c].name, ne); pass++;
        } else {
            printf("  FAIL: %s data mismatch\n", cases[c].name); fail++;
        }
        free(got);
    }

    /* cleanup */
    for (int c = 0; c < NC; c++) { free(capo_bufs[c]); free(refs[c]); }

    printf("\n=== %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
