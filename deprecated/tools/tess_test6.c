#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

static inline uint32_t tess_stride_scatter_local(uint32_t weight_idx) {
    return (weight_idx * 37u) % 20736u;
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stderr, "START argc=%d\n", argc);
    if (argc < 2) return 1;
    GgufReader reader;
    fprintf(stderr, "opening...\n"); fflush(stderr);
    if (gguf_open(argv[1], &reader) != 0) { fprintf(stderr, "FAIL\n"); return 1; }
    fprintf(stderr, "OK n=%u\n", reader.n_tensors);

    const uint32_t MAX_TENSOR = 32u * 1024 * 1024;
    uint8_t *tb = (uint8_t *)malloc(MAX_TENSOR);
    uint8_t *tt = (uint8_t *)malloc((uint64_t)MAX_TENSOR + 256*1024);
    fprintf(stderr, "tb=%p tt=%p\n", tb, tt);

    /* Process only first 3 tensors to isolate the crash */
    for (uint32_t i = 0; i < 3 && i < reader.n_tensors; i++) {
        const char *name = reader.names[i];
        uint8_t dtype = reader.dtypes[i];
        uint32_t tsize = reader.sizes[i];
        uint32_t csize = (dtype < 16) ? GGUF_CELL_SIZE[dtype] : 0;
        uint64_t n_elems = 1;
        for (uint8_t d = 0; d < reader.n_dims[i]; d++)
            n_elems *= reader.dims[i * 4 + d];
        fprintf(stderr, "[%u] %s type=%u cell=%u size=%u n=%lu\n",
               i, name, dtype, csize, tsize, (unsigned long)n_elems);
        if (csize == 0 || tsize > MAX_TENSOR) { fprintf(stderr, " SKIP\n"); continue; }

        fprintf(stderr, "  reading tensor...\n");
        int rc = gguf_read_tensor(argv[1], &reader, i, tb, tsize);
        fprintf(stderr, "  read=%d\n", rc);
        if (rc != 0) continue;

        uint32_t cube_bytes = 20736u * csize;
        fprintf(stderr, "  cube_bytes=%u (alloc=%u)\n", cube_bytes, MAX_TENSOR + 256*1024);
        if (cube_bytes > MAX_TENSOR) { fprintf(stderr, "  cube too large\n"); continue; }

        /* Build header */
        fprintf(stderr, "  building header...\n");
        TESS_Header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = GEO_TESS_MAGIC;
        hdr.version = TESS_VERSION;
        hdr.total_slots = 20736;
        hdr.cell_size = csize;
        hdr.scale_factor = 65536;
        hdr.x_slots = 6912;
        hdr.y_slots = 6912;
        hdr.z_slots = 6912;
        hdr.tensor_count = (uint32_t)n_elems;
        fprintf(stderr, "  header OK\n");

        /* Build formula */
        fprintf(stderr, "  building formula...\n");
        TESS_Formula fml;
        memset(&fml, 0, sizeof(fml));
        fml.mirror_axis_x = 6912;
        fml.mirror_axis_y = 6912;
        fml.mirror_axis_z = 6912;
        fml.stride_seed = 37;
        fprintf(stderr, "  formula OK\n");

        /* Write header + formula */
        uint8_t *p = tt;
        memcpy(p, &hdr, 64); p += 64;
        memcpy(p, &fml, 64); p += 64;

        /* Scatter */
        fprintf(stderr, "  scatter %u elements into %u bytes...\n", (uint32_t)n_elems, cube_bytes);
        memset(p, 0, cube_bytes);
        const uint8_t *src = tb;
        uint32_t n32 = (uint32_t)n_elems;
        for (uint32_t j = 0; j < n32; j++) {
            uint32_t slot = tess_stride_scatter_local(j);
            uint32_t off = slot * csize;
            if (off + csize <= cube_bytes)
                memcpy(p + off, src + (uint64_t)j * csize, csize);
        }
        fprintf(stderr, "  scatter done\n");

        p += cube_bytes;

        /* CRC */
        fprintf(stderr, "  CRC on %u bytes...\n", cube_bytes);
        uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
        const uint64_t poly = 0x42F0E1EBA9EA3693ULL;
        for (uint64_t k = 0; k < cube_bytes; k++) {
            crc ^= (uint64_t)p[k] << 56;
            for (int j = 0; j < 8; j++)
                crc = (crc & (1ULL << 63)) ? ((crc << 1) ^ poly) : (crc << 1);
        }
        crc ^= 0xFFFFFFFFFFFFFFFFULL;
        memcpy(p, &crc, 8);
        p += 8;

        int32_t total = (int32_t)(p - tt);
        fprintf(stderr, "  total=%d bytes\n", total);
        fprintf(stderr, "  PASS [%u]\n", i);
    }

    free(tb); free(tt);
    gguf_close(&reader);
    fprintf(stderr, "DONE\n");
    return 0;
}
