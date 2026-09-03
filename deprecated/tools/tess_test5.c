#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

static int32_t tess_bake_encode(const void *src, uint32_t n_elems,
                                 uint32_t cell_size, uint32_t gguf_type,
                                 uint32_t capo_id, uint32_t capo_total,
                                 void *dst, uint32_t dst_cap)
{
    uint32_t cube_bytes = TESS_TOTAL_SLOTS * cell_size;
    uint32_t payload_size = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;
    if (dst_cap < payload_size) return -1;
    uint8_t *p = (uint8_t *)dst;
    TESS_Header hdr;
    tess_header_init(&hdr, gguf_type, cell_size);
    hdr.scale_factor = 65536u;
    hdr.x_slots = TESS_X_SLOTS;
    hdr.y_slots = TESS_Y_SLOTS;
    hdr.z_slots = TESS_Z_SLOTS;
    hdr.tensor_count = n_elems;
    memcpy(p, &hdr, TESS_HEADER_SIZE);
    p += TESS_HEADER_SIZE;
    TESS_Formula fml;
    tess_formula_init(&fml);
    fml.mirror_axis_x = hdr.x_slots;
    fml.mirror_axis_y = hdr.y_slots;
    fml.mirror_axis_z = hdr.z_slots;
    fml.stride_seed = TESS_STRIDE_37;
    fml.capo_id    = capo_id;
    fml.capo_total = (uint8_t)capo_total;
    memcpy(p, &fml, TESS_FORMULA_SIZE);
    p += TESS_FORMULA_SIZE;
    uint8_t *cube_data = p;
    memset(cube_data, 0, cube_bytes);
    const uint8_t *src_bytes = (const uint8_t *)src;
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = tess_stride_scatter(i);
        if (slot >= TESS_TOTAL_SLOTS) slot = i % TESS_TOTAL_SLOTS;
        uint32_t off = slot * cell_size;
        if (off + cell_size <= cube_bytes)
            memcpy(cube_data + off, src_bytes + (uint64_t)i * cell_size, cell_size);
    }
    p += cube_bytes;
    uint64_t cube_crc = tess_crc64(cube_data, cube_bytes);
    memcpy(p, &cube_crc, TESS_CRC_SIZE);
    p += TESS_CRC_SIZE;
    ((TESS_Header *)dst)->cube_checksum = cube_crc;
    return (int32_t)(p - (uint8_t *)dst);
}

int main(int argc, char **argv) {
    fprintf(stderr, "START\n");
    if (argc < 2) return 1;
    GgufReader reader;
    if (gguf_open(argv[1], &reader) != 0) { fprintf(stderr, "FAIL\n"); return 1; }
    fprintf(stderr, "OK n=%u\n", reader.n_tensors);
    const uint32_t MAX_TENSOR = 32u * 1024 * 1024;
    uint8_t *tb = (uint8_t *)malloc(MAX_TENSOR);
    uint8_t *tt = (uint8_t *)malloc((uint64_t)MAX_TENSOR + 256*1024);
    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        const char *name = reader.names[i];
        uint8_t dtype = reader.dtypes[i];
        uint32_t tsize = reader.sizes[i];
        uint32_t csize = (dtype < 16) ? GGUF_CELL_SIZE[dtype] : 0;
        uint64_t n_elems = 1;
        for (uint8_t d = 0; d < reader.n_dims[i]; d++)
            n_elems *= reader.dims[i * 4 + d];
        fprintf(stderr, "[%u] %s type=%u cell=%u size=%u\n", i, name, dtype, csize, tsize);
        if (csize == 0 || tsize > MAX_TENSOR) continue;
        int rc = gguf_read_tensor(argv[1], &reader, i, tb, tsize);
        if (rc != 0) { fprintf(stderr, " read=%d\n", rc); continue; }
        fprintf(stderr, "  encoding n=%lu cell=%u...\n", (unsigned long)n_elems, csize);
        int32_t enc = tess_bake_encode(tb, (uint32_t)n_elems, csize, dtype,
                                       0, 1,
                                       tt, (uint32_t)((uint64_t)MAX_TENSOR + 256*1024));
        fprintf(stderr, "  enc=%d\n", enc);
        if (i > 2) break;
    }
    free(tb); free(tt);
    gguf_close(&reader);
    fprintf(stderr, "DONE\n");
    return 0;
}
