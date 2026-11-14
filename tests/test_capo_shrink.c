/* test_capo_shrink.c — v2 capo: last capo scatters over what it holds, no padding.
 *
 * v1 padded every capo to the full 20736-cell field; on a real model that is
 * 10.4% of the pack. v2 writes the last capo at exactly tensor_count cells,
 * legal only when stride 37 is still a permutation over that modulus.
 * This checks the reader against a buffer built by the same rule as the encoder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geo_tess_container.h"

static int fails;

static void ck(int cond, const char *what) {
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

/* build a capo exactly the way tess_gguf_pack.c encode_capo does */
static uint32_t build_capo(uint8_t *dst, const uint8_t *src, uint32_t n_elems,
                           uint32_t cell_size, uint32_t gguf_type) {
    TESS_Header hdr;
    tess_header_init(&hdr, gguf_type, cell_size);
    hdr.scale_factor = 65536u;
    uint32_t eff = tess_effective_slots(&hdr);
    if (n_elems < eff && (n_elems % V6_STRIDE) != 0u) {
        eff = n_elems;
        hdr.version = 2u;
    }
    uint32_t cube_bytes = eff * cell_size;
    hdr.x_slots = TESS_X_SLOTS; hdr.y_slots = TESS_Y_SLOTS; hdr.z_slots = TESS_Z_SLOTS;
    hdr.tensor_count = n_elems;

    uint8_t *p = dst;
    memcpy(p, &hdr, TESS_HEADER_SIZE); p += TESS_HEADER_SIZE;
    TESS_Formula fml;
    tess_formula_init(&fml);
    fml.mirror_axis_x = hdr.x_slots; fml.mirror_axis_y = hdr.y_slots;
    fml.mirror_axis_z = hdr.z_slots;
    fml.stride_seed = TESS_STRIDE_37;
    memcpy(p, &fml, TESS_FORMULA_SIZE); p += TESS_FORMULA_SIZE;

    uint8_t *cube = p;
    memset(cube, 0, cube_bytes);
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = tess_stride_scatter_in(i, eff);
        memcpy(cube + (uint64_t)slot * cell_size, src + (uint64_t)i * cell_size, cell_size);
    }
    p += cube_bytes;
    uint64_t crc = tess_crc64(cube, cube_bytes);
    memcpy(p, &crc, TESS_CRC_SIZE);
    ((TESS_Header *)dst)->cube_checksum = crc;
    return (uint32_t)(p + TESS_CRC_SIZE - dst);
}

/* read every element back through the public reader path */
static int read_back(uint8_t *buf, uint32_t sz, uint32_t n_elems, uint32_t cell_size,
                     uint8_t *out) {
    TESS_CapoReader r;
    if (tess_capo_open_buf(&r, buf, sz) != 0) return -1;
    if (r.n_elems != n_elems) return -2;
    for (uint32_t i = 0; i < n_elems; i++)
        if (tess_capo_load_elem(&r, i, out + (uint64_t)i * cell_size) != (int)cell_size)
            return -3;
    return 0;
}

static void run(const char *label, uint32_t n_elems, uint32_t cell_size,
                uint32_t gguf_type, int expect_shrink) {
    uint32_t full = TESS_TOTAL_SLOTS * cell_size;
    uint8_t *src = (uint8_t *)malloc((size_t)n_elems * cell_size);
    uint8_t *buf = (uint8_t *)malloc(full + 4096);
    uint8_t *out = (uint8_t *)malloc((size_t)n_elems * cell_size);
    for (uint32_t i = 0; i < n_elems * cell_size; i++) src[i] = (uint8_t)(i * 31 + 7);

    uint32_t sz = build_capo(buf, src, n_elems, cell_size, gguf_type);
    const TESS_Header *h = (const TESS_Header *)buf;
    int rc = read_back(buf, sz, n_elems, cell_size, out);
    int exact = (rc == 0) && memcmp(src, out, (size_t)n_elems * cell_size) == 0;

    printf("  %-22s n=%6u cell=%3u  ver=%u  size=%7u (full=%7u)  shrink=%s  exact=%s\n",
           label, n_elems, cell_size, h->version, sz, full,
           sz < full ? "yes" : "no", exact ? "yes" : "NO");
    ck(exact, "roundtrip through the v2 reader is exact");
    if (expect_shrink) {
        ck(sz < full, "capo is smaller than the full field");
        ck(sz == TESS_HEADER_SIZE + TESS_FORMULA_SIZE + n_elems * cell_size + TESS_CRC_SIZE,
           "capo size == header + formula + n*cell + crc");
    } else {
        ck(sz == TESS_HEADER_SIZE + TESS_FORMULA_SIZE + full + TESS_CRC_SIZE,
           "capo keeps the full field when shrinking is illegal");
    }
    free(src); free(buf); free(out);
}

int main(void) {
    printf("capo shrink (v2): last capo scatters over tensor_count\n");
    /* 4352 = 25088 - 20736, the real tail of a Qwen2.5 attn_q tensor */
    run("tail of attn_q", 4352, 34, 8, 1);
    run("tail, Q4_0 cell", 3584, 18, 2, 1);
    run("full capo", TESS_TOTAL_SLOTS, 34, 8, 0);
    run("tiny tensor", 100, 34, 8, 1);
    /* n % 37 == 0 -> stride 37 would collide, must keep the full field */
    run("n divisible by 37", 111, 34, 8, 0);
    run("n=37", 37, 34, 8, 0);
    if (fails) { printf("FAILED: %d checks\n", fails); return 1; }
    printf("PASS: capo shrink\n");
    return 0;
}
