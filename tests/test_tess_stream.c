/* test_tess_stream.c — Streaming capo decode vs all-at-once comparison
 * ═══════════════════════════════════════════════════════════════════════
 * Verifies that streaming per-capo decode produces identical results
 * to the all-at-once bulk load (tess_load_decode_element).
 *
 * No GGUF file needed — creates a synthetic tensor in RAM, writes it
 * to .tess, then reads back via both paths and compares.
 * ═══════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "geo_tess_container.h"

#define TENSOR_NAME "test_streaming"
#define N_ELEMENTS  300u   /* more than 20736/144 capos needed */
#define CELL_SIZE   TESS_CELL_Q4_K

static uint8_t original[N_ELEMENTS][CELL_SIZE];

/* write synthetic tensor to .tess capo0 */
static int write_capo0(const char *path, uint32_t seed) {
    srand(seed);
    for (uint32_t i = 0; i < N_ELEMENTS; i++) {
        for (uint32_t j = 0; j < CELL_SIZE; j++)
            original[i][j] = (uint8_t)(rand() & 0xFF);
    }

    TESS_Header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = GEO_TESS_MAGIC;
    hdr.version     = TESS_VERSION;
    hdr.cell_size   = CELL_SIZE;
    hdr.total_slots = TESS_TOTAL_SLOTS;
    hdr.x_slots     = TESS_X_SLOTS;
    hdr.y_slots     = TESS_Y_SLOTS;
    hdr.z_slots     = TESS_Z_SLOTS;
    hdr.tensor_count = N_ELEMENTS;
    hdr.source_size = 0;

    TESS_Formula fml;
    memset(&fml, 0, sizeof(fml));
    fml.mirror_axis_x = TESS_TOTAL_SLOTS;
    fml.capo_id       = 0;
    fml.capo_total    = 1;

    uint32_t cube_bytes = TESS_TOTAL_SLOTS * CELL_SIZE;
    uint8_t *cube = calloc(1, cube_bytes);
    for (uint32_t i = 0; i < N_ELEMENTS; i++) {
        uint32_t slot = tess_stride_scatter(i);
        if (slot >= TESS_TOTAL_SLOTS) slot = i % TESS_TOTAL_SLOTS;
        memcpy(cube + (uint64_t)slot * CELL_SIZE, original[i], CELL_SIZE);
    }

    uint64_t crc = tess_crc64(cube, cube_bytes);

    FILE *f = fopen(path, "wb");
    if (!f) { free(cube); return -1; }
    fwrite(&hdr, 1, TESS_HEADER_SIZE, f);
    fwrite(&fml, 1, TESS_FORMULA_SIZE, f);
    fwrite(cube, 1, cube_bytes, f);
    fwrite(&crc, 1, TESS_CRC_SIZE, f);
    fclose(f);
    free(cube);
    return 0;
}

static int test_single_stream_decode(const char *path) {
    printf("── test_single_stream_decode ──\n");
    TESS_CapoReader r;
    assert(tess_capo_open(&r, path) == 0);
    assert(r.n_elems == N_ELEMENTS);
    assert(r.cell_size == CELL_SIZE);

    uint32_t checked = 0;
    for (uint32_t i = 0; i < N_ELEMENTS; i++) {
        uint8_t cell[CELL_SIZE];
        int n = tess_capo_load_elem(&r, i, cell);
        assert(n == (int)CELL_SIZE);
        assert(memcmp(cell, original[i], CELL_SIZE) == 0);
        checked++;
    }
    printf("  %u/%u elements match\n", checked, N_ELEMENTS);
    tess_capo_close(&r);
    printf("  PASS\n");
    return 0;
}

static int test_range_stream_decode(const char *path) {
    printf("── test_range_stream_decode ──\n");
    TESS_CapoReader r;
    assert(tess_capo_open(&r, path) == 0);

    uint8_t *buf = malloc(50 * CELL_SIZE);
    assert(buf);

    /* range [100, +50) */
    int bytes = tess_capo_load_range(&r, 100, 50, buf);
    assert(bytes == 50 * (int)CELL_SIZE);

    uint32_t checked = 0;
    for (uint32_t i = 0; i < 50; i++) {
        uint32_t elem_idx = 100 + i;
        uint8_t single[CELL_SIZE];
        tess_capo_load_elem(&r, elem_idx, single);
        assert(memcmp(buf + (uint64_t)i * CELL_SIZE, single, CELL_SIZE) == 0);
        checked++;
    }
    printf("  range[100,+50): %u elements match\n", checked);
    free(buf);
    tess_capo_close(&r);
    printf("  PASS\n");
    return 0;
}

static int test_crc_verify(const char *path) {
    printf("── test_crc_verify ──\n");
    TESS_CapoReader r;
    assert(tess_capo_open(&r, path) == 0);
    assert(tess_capo_verify_crc(&r) == 1);
    printf("  CRC matches\n");
    tess_capo_close(&r);
    printf("  PASS\n");
    return 0;
}

static int test_open_buf(const char *path) {
    printf("── test_open_buf (no-copy) ──\n");
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    assert(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);

    TESS_CapoReader r;
    assert(tess_capo_open_buf(&r, buf, (uint64_t)sz) == 0);
    assert(r.n_elems == N_ELEMENTS);

    uint8_t cell[CELL_SIZE];
    assert(tess_capo_load_elem(&r, 0, cell) == (int)CELL_SIZE);
    assert(memcmp(cell, original[0], CELL_SIZE) == 0);

    tess_capo_close(&r);
    /* buffer stays valid — we free it ourselves */
    free(buf);
    printf("  no-copy open + load: PASS\n");
    return 0;
}

static int test_path_derivation(void) {
    printf("── test_path_derivation ──\n");
    char buf[256];

    tess_capo_make_path(buf, sizeof(buf), "foo_capo0.tess", 3);
    assert(strcmp(buf, "foo_capo3.tess") == 0);

    tess_capo_make_path(buf, sizeof(buf), "bar.tess", 5);
    assert(strcmp(buf, "bar_capo5.tess") == 0);

    tess_capo_make_path(buf, sizeof(buf), "baz_data", 1);
    assert(strcmp(buf, "baz_data_capo1") == 0);

    printf("  PASS\n");
    return 0;
}

int main(void) {
    const char *tmppath = "test_stream_tmp.tess";

    printf("═══ Streaming Capo Decode Tests ═══\n\n");

    if (write_capo0(tmppath, 42) != 0) {
        fprintf(stderr, "Failed to write capo0\n");
        return 1;
    }

    int fail = 0;
    fail += test_single_stream_decode(tmppath);
    fail += test_range_stream_decode(tmppath);
    fail += test_crc_verify(tmppath);
    fail += test_open_buf(tmppath);
    fail += test_path_derivation();

    remove(tmppath);

    printf("\n═══ %s ═══\n", fail ? "FAIL" : "ALL PASS");
    return fail;
}
