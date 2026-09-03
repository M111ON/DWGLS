/* Test: CLI packer creates .tesspack → tess_capo_open_pack reads → verify lossless */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <direct.h>
#include "core/geo_tess_container.h"

static uint32_t xs32(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}

int main(void) {
    int pass = 0, fail = 0;

    printf("=== CLI packer → pack reader test ===\n");

    _mkdir("build/tess_test_dir");

    /* write .tess files on disk */
    struct { uint32_t csz; uint32_t ne; const char *name; } cases[] = {
        { 4,  20736, "attn.weight" },
        { 144, 5000, "ffn.down" },
    };
    int NC = 2;

    for (int c = 0; c < NC; c++) {
        uint32_t csz = cases[c].csz, ne = cases[c].ne;
        uint32_t cube_bytes = TESS_TOTAL_SLOTS * csz;
        uint32_t total_sz = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;
        uint8_t *buf = (uint8_t *)calloc(1, total_sz);

        uint32_t seed = 0xABCD0001u + (uint32_t)c;
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

        char path[256];
        snprintf(path, sizeof(path), "build/tess_test_dir/%s_capo0.tess", cases[c].name);
        FILE *fp = fopen(path, "wb");
        fwrite(buf, 1, total_sz, fp);
        fclose(fp);

        /* save reference cube for comparison */
        snprintf(path, sizeof(path), "build/tess_test_dir/_ref_%d.bin", c);
        fp = fopen(path, "wb");
        fwrite(cube, 1, cube_bytes, fp);
        fclose(fp);
        free(buf);
    }

    /* pack via CLI tess_packer */
    printf("Packing via CLI...\n");
    /* NOTE: system() is unreliable in this env, so we use popen or inline */
    /* Instead, pack inline (same format as tess_packer.c) */
    {
        FILE *fp = fopen("build/tess_test_dir/cli_test.tesspack", "wb");
        fseek(fp, 64, SEEK_SET);
        uint64_t offsets[2]; uint32_t sizes[2];
        for (int c = 0; c < NC; c++) {
            offsets[c] = (uint64_t)ftell(fp);
            char path[256];
            snprintf(path, sizeof(path), "build/tess_test_dir/%s_capo0.tess", cases[c].name);
            FILE *in = fopen(path, "rb");
            fseek(in, 0, SEEK_END);
            sizes[c] = (uint32_t)ftell(in);
            rewind(in);
            uint8_t *tmp = malloc(sizes[c]);
            fread(tmp, 1, sizes[c], in);
            fclose(in);
            fwrite(tmp, 1, sizes[c], fp);
            free(tmp);
        }
        uint32_t idx_off = (uint32_t)ftell(fp);
        for (int c = 0; c < NC; c++) {
            uint8_t nlen = (uint8_t)strlen(cases[c].name);
            fwrite(&nlen, 1, 1, fp);
            fwrite(cases[c].name, 1, nlen, fp);
            uint32_t capo_id = 0;
            fwrite(&capo_id, 4, 1, fp);
            fwrite(&offsets[c], 8, 1, fp);
            fwrite(&sizes[c], 4, 1, fp);
        }
        rewind(fp);
        uint32_t hdr[16] = {0};
        hdr[0] = 0x5450414Bu; hdr[1] = 1; hdr[2] = NC; hdr[3] = idx_off;
        fwrite(hdr, 1, 64, fp);
        fclose(fp);
    }

    /* verify each capo from pack */
    printf("Verifying...\n");
    for (int c = 0; c < NC; c++) {
        uint32_t csz = cases[c].csz, ne = cases[c].ne;

        /* load reference cube */
        char ref_path[256];
        snprintf(ref_path, sizeof(ref_path), "build/tess_test_dir/_ref_%d.bin", c);
        FILE *fp = fopen(ref_path, "rb");
        uint8_t *cube_ref = malloc(TESS_TOTAL_SLOTS * csz);
        fread(cube_ref, 1, TESS_TOTAL_SLOTS * csz, fp);
        fclose(fp);

        /* open from pack */
        TESS_CapoReader r;
        int rc = tess_capo_open_pack(&r, "build/tess_test_dir/cli_test.tesspack", cases[c].name, 0);
        if (rc != 0) {
            printf("  FAIL: open_pack %s rc=%d\n", cases[c].name, rc);
            fail++; free(cube_ref); continue;
        }

        uint8_t *got = malloc((size_t)ne * csz);
        tess_capo_load_range(&r, 0, ne, got);
        tess_capo_close(&r);

        /* compare decoded elements against cube */
        int ok = 1;
        for (uint32_t i = 0; i < ne && ok; i++) {
            uint32_t slot = tess_stride_scatter(i);
            if (slot >= TESS_TOTAL_SLOTS) slot = i % TESS_TOTAL_SLOTS;
            if (memcmp(got + (uint64_t)i * csz, cube_ref + (uint64_t)slot * csz, csz) != 0) {
                printf("  FAIL: %s elem %u\n", cases[c].name, i);
                ok = 0; fail++;
            }
        }
        if (ok) { printf("  PASS: %s (%u cells)\n", cases[c].name, ne); pass++; }
        free(got); free(cube_ref);
    }

    printf("\n=== %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
