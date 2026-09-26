/* test_tesspack_tiles.c — consumer A (serve primitive): tess-unit I/O.
 * One full 20736-slot capo in a pack; flat load vs 18 tess-tile loads must
 * match byte-for-byte. Selective tile read proves serving less than whole.
 * Oracle: xorshift pattern + flat load. Self-contained (no model file).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/geo_tess_container.h"
#include "core/geo_tesseract_addr.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static uint32_t xs32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

#define CSZ 4u
#define NSLOT 20736u
#define PACK "build/test_tiles.tesspack"
#define TNAME "layer.0.weight"

int main(void) {
    uint32_t cube_bytes = NSLOT * CSZ;
    uint32_t total_sz = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;
    uint8_t *buf = (uint8_t *)calloc(1, total_sz);
    if (!buf) return 1;

    uint32_t seed = 0x71EE0001u;
    uint8_t *cube = buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
    for (uint32_t i = 0; i < NSLOT; i++) {
        uint8_t *dst = cube + (uint64_t)tess_stride_scatter(i) * CSZ;
        for (uint32_t b = 0; b < CSZ; b++) dst[b] = (uint8_t)(xs32(&seed) & 0xFF);
    }
    TESS_Header *h = (TESS_Header *)buf;
    tess_header_init(h, 0, CSZ);
    h->tensor_count = NSLOT;
    TESS_Formula *f = (TESS_Formula *)(buf + TESS_HEADER_SIZE);
    tess_formula_init(f);
    uint64_t crc = tess_crc64(cube, cube_bytes);
    memcpy(cube + cube_bytes, &crc, TESS_CRC_SIZE);

    FILE *fp = fopen(PACK, "wb");
    if (!fp) { printf("FAIL: pack create\n"); return 1; }
    fseek(fp, 64, SEEK_SET);
    uint64_t off = (uint64_t)ftell(fp);
    fwrite(buf, 1, total_sz, fp);
    uint32_t index_offset = (uint32_t)ftell(fp);
    uint8_t nlen = (uint8_t)strlen(TNAME);
    uint32_t capo_id = 0;
    fwrite(&nlen, 1, 1, fp);
    fwrite(TNAME, 1, nlen, fp);
    fwrite(&capo_id, 4, 1, fp);
    fwrite(&off, 8, 1, fp);
    fwrite(&total_sz, 4, 1, fp);
    uint32_t hdr[16] = {0};
    hdr[0] = 0x5450414Bu; hdr[1] = 1; hdr[2] = 1; hdr[3] = index_offset;
    rewind(fp);
    fwrite(hdr, 1, 64, fp);
    fclose(fp);
    free(buf);

    static uint8_t flat[20736u * 4u];
    static uint8_t tiles[20736u * 4u];
    TESS_CapoReader r;

    /* T1: flat load. */
    CHECK(tess_capo_open_pack(&r, PACK, TNAME, 0) == 0, "open pack");
    CHECK(tess_capo_load_range(&r, 0, NSLOT, flat) == NSLOT * CSZ, "flat load");
    tess_capo_close(&r);

    /* T2: 18 tess tiles, one range each, concat == flat. */
    CHECK(tess_capo_open_pack(&r, PACK, TNAME, 0) == 0, "reopen pack");
    for (uint32_t t = 0; t < 18u; t++) {
        uint32_t got = tess_capo_load_range(&r, t * 1152u, 1152u,
                                            tiles + (size_t)t * 1152u * CSZ);
        if (got != 1152u * CSZ) { printf("FAIL: tile %u\n", t); fails++; break; }
    }
    tess_capo_close(&r);
    CHECK(memcmp(flat, tiles, sizeof(flat)) == 0, "18 tiles == flat");

    /* T3: view reads on the flat buffer land on tile bytes. */
    for (uint32_t t = 0; t < 18u; t += 7) {
        const uint8_t *p = tess_capo_at(flat, CSZ, t, 3, 100);
        const uint8_t *q = tiles + ((size_t)t * 1152u + 3u * 144u + 100u) * CSZ;
        if (!p || memcmp(p, q, CSZ) != 0) {
            printf("FAIL: view tile %u\n", t); fails++; break;
        }
    }

    /* T4: single-tile selective read == flat slice (serve less than whole). */
    static uint8_t one[1152u * 4u];
    CHECK(tess_capo_open_pack(&r, PACK, TNAME, 0) == 0, "reopen2");
    CHECK(tess_capo_load_range(&r, 5 * 1152u, 1152u, one) == 1152u * CSZ, "tile 5");
    tess_capo_close(&r);
    CHECK(memcmp(one, flat + (size_t)5 * 1152u * CSZ, sizeof(one)) == 0, "tile5 slice");

    remove(PACK);
    if (!fails) printf("tesspack_tiles: ALL PASS\n");
    return fails ? 1 : 0;
}
