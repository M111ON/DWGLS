/* tess_bake.c — GGUF → .tess encode tool
 *
 * Reads a GGUF file and encodes each tensor into a separate .tess file
 * using stride-37 scatter + CRC-64 integrity.
 *
 * Usage: tess_bake <model.gguf> [output_dir]
 *   Reads GGUF, writes <output_dir>/<name>.tess per tensor
 *
 * BUILD: gcc -O2 -Wall -Wno-unused-parameter -Icore -o tess_bake tess_bake.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gguf_reader.h"
#include "geo_tess_container.h"

/* GGUF type → cell size (block bytes for tess encoding) */
static uint32_t gguf_cell_size(uint32_t dtype) {
    static const uint32_t table[] = {
        4,   /* F32  = 0 */
        2,   /* F16  = 1 */
        18,  /* Q4_0 = 2 */
        20,  /* Q4_1 = 3 */
        0, 0,
        22,  /* Q5_0 = 6 */
        24,  /* Q5_1 = 7 */
        34,  /* Q8_0 = 8 */
        36,  /* Q8_1 = 9 */
        84,  /* Q2_K = 10 */
        110, /* Q3_K = 11 */
        144, /* Q4_K = 12 */
        176, /* Q5_K = 13 */
        210, /* Q6_K = 14 */
        292, /* Q8_K = 15 */
    };
    if (dtype < sizeof(table)/sizeof(table[0])) return table[dtype];
    if (dtype == 41) return 6;   /* Q1_0: 2 fp16 + 4 bytes (32×1-bit) */
    return 0;
}

static const char *GGUF_TYPE_NAME[] = {
    "F32","F16","Q4_0","Q4_1","rem4","rem5",
    "Q5_0","Q5_1","Q8_0","Q8_1","Q2_K","Q3_K",
    "Q4_K","Q5_K","Q6_K","Q8_K",
};

static const char *type_name(uint32_t dt) {
    return (dt < 16) ? GGUF_TYPE_NAME[dt] : "???";
}

static const char *basename_of(const char *path) {
    const char *p = strrchr(path, '\\');
    if (!p) p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* Inline tess encode — avoids pulling in codec_tess.h → dwgls_codec.h chain */
static int32_t tess_bake_encode(const void *src, uint32_t n_elems,
                                 uint32_t cell_size, uint32_t gguf_type,
                                 uint32_t capo_id, uint32_t capo_total,
                                 uint32_t scale_w, uint32_t axis_id, uint32_t axis_position,
                                 void *dst, uint32_t dst_cap)
{
    if (dst_cap < 256) return -1;

    uint8_t *p = (uint8_t *)dst;

    /* Header */
    TESS_Header hdr;
    tess_header_init(&hdr, gguf_type, cell_size);
    hdr.scale_factor = 65536u;
    if (scale_w > 0) {
        double s = exp2(-(double)scale_w / 12.0);
        hdr.scale_factor = (uint32_t)(s * 65536.0 + 0.5);
    }
    hdr.x_slots = TESS_X_SLOTS;
    hdr.y_slots = TESS_Y_SLOTS;
    hdr.z_slots = TESS_Z_SLOTS;
    hdr.tensor_count = n_elems;
    memcpy(p, &hdr, TESS_HEADER_SIZE);
    p += TESS_HEADER_SIZE;

    /* Formula */
    TESS_Formula fml;
    tess_formula_init(&fml);
    fml.mirror_axis_x = hdr.x_slots;
    fml.mirror_axis_y = hdr.y_slots;
    fml.mirror_axis_z = hdr.z_slots;
    fml.stride_seed = TESS_STRIDE_37;
    fml.capo_id    = capo_id;
    fml.capo_total = (uint8_t)capo_total;
    fml.axis_id = (uint8_t)axis_id;
    fml.axis_position = axis_position;
    memcpy(p, &fml, TESS_FORMULA_SIZE);
    p += TESS_FORMULA_SIZE;

    /* Scatter through stride-37 (scale-aware) */
    uint32_t eff_slots = tess_effective_slots(&hdr);
    uint32_t cube_bytes = eff_slots * cell_size;
    uint32_t payload_size = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;
    if (dst_cap < payload_size) return -1;

    uint8_t *cube_data = p;
    memset(cube_data, 0, cube_bytes);
    const uint8_t *src_bytes = (const uint8_t *)src;
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = tess_stride_scatter_in(i, eff_slots);
        uint32_t off = slot * cell_size;
        if (off + cell_size <= cube_bytes)
            memcpy(cube_data + off, src_bytes + (uint64_t)i * cell_size, cell_size);
    }
    p += cube_bytes;

    /* CRC-64 */
    uint64_t cube_crc = tess_crc64(cube_data, cube_bytes);
    memcpy(p, &cube_crc, TESS_CRC_SIZE);
    p += TESS_CRC_SIZE;

    ((TESS_Header *)dst)->cube_checksum = cube_crc;
    return (int32_t)(p - (uint8_t *)dst);
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stderr, "tess_bake START argc=%d\n", argc);
    if (argc < 2) {
        fprintf(stderr, "Usage: tess_bake <model.gguf> [output_dir] [--scale W] [--axis N] [--position P]\n");
        return 1;
    }

    const char *gguf_path = argv[1];
    const char *out_dir = (argc > 2) ? argv[2] : ".";
    uint32_t scale_w = 0;  /* default: home (W=0, s=1.0) */
    uint32_t axis_id = 0;  /* default: X axis */
    uint32_t axis_position = 0;  /* default: position 0 */

    /* Parse optional --scale W, --axis N, --position P */
    for (int a = 3; a < argc; a++) {
        if (strcmp(argv[a], "--scale") == 0 && a + 1 < argc) {
            scale_w = (uint32_t)atoi(argv[a + 1]) % 144;
            a++;
        } else if (strcmp(argv[a], "--axis") == 0 && a + 1 < argc) {
            axis_id = (uint32_t)atoi(argv[a + 1]) % 6;
            a++;
        } else if (strcmp(argv[a], "--position") == 0 && a + 1 < argc) {
            axis_position = (uint32_t)atoi(argv[a + 1]);
            a++;
        }
    }

    fprintf(stderr, "tess_bake: scale W=%u (s=%.4f), axis=%u, position=%u\n", scale_w,
            exp2(-(double)scale_w / 12.0), axis_id, axis_position);

    GgufReader reader;
    fprintf(stderr, "opening %s\n", gguf_path);
    if (gguf_open(gguf_path, &reader) != 0) {
        fprintf(stderr, "ERROR: cannot open %s\n", gguf_path);
        return 1;
    }

    fprintf(stderr, "tess_bake: %s\n", gguf_path);
    fprintf(stderr, "  Tensors: %u\n", reader.n_tensors);
    fprintf(stderr, "  Output:  %s/\n\n", out_dir);

#if defined(_WIN32)
    mkdir(out_dir);
#else
    mkdir(out_dir, 0755);
#endif

    fprintf(stderr, "allocating encode buffer...\n");
    const uint32_t MAX_TENSOR = 32u * 1024 * 1024;
    uint8_t *tess_buf = (uint8_t *)malloc((uint64_t)MAX_TENSOR + 256 * 1024);
    fprintf(stderr, "tess_buf=%p\n", tess_buf);
    if (!tess_buf) {
        fprintf(stderr, "ERROR: alloc failed\n");
        gguf_close(&reader);
        return 1;
    }

    uint64_t total_encoded = 0;
    uint32_t encoded_count = 0, skipped_count = 0;

    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        const char *name = reader.names[i];
        uint8_t dtype = reader.dtypes[i];
        uint32_t tsize = reader.sizes[i];

        uint32_t csize = gguf_cell_size(dtype);
        uint64_t n_elems = 1;
        for (uint8_t d = 0; d < reader.n_dims[i]; d++)
            n_elems *= reader.dims[i * 4 + d];

        fprintf(stderr, "  [%3u] %-40s  %4s  %6lu elems  %6u bytes",
               i, name, type_name(dtype), (unsigned long)n_elems, tsize);
        fflush(stderr);

        if (csize == 0) {
            fprintf(stderr, "  SKIP (type %u)\n", dtype);
            skipped_count++;
            continue;
        }
        /* n_elems = number of cells (blocks) in raw data, not dimension product */
        uint32_t n_blocks = tsize / csize;
        uint32_t n_capos = (n_blocks + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;
        if (n_capos < 1) n_capos = 1;
        fprintf(stderr, "  encoding n_blocks=%u cell=%u capos=%u...\n",
               n_blocks, csize, n_capos);

        /* read directly from mmap per-capo — no 500MB buffer needed */
        uint8_t *tensor_mmap = reader.base + reader.data_offset + reader.offsets[i];

        for (uint32_t c = 0; c < n_capos; c++) {
            uint32_t offset = c * TESS_TOTAL_SLOTS;
            uint32_t chunk = n_blocks - offset;
            if (chunk > TESS_TOTAL_SLOTS) chunk = TESS_TOTAL_SLOTS;

            int32_t enc_sz = tess_bake_encode(tensor_mmap + (uint64_t)offset * csize,
                                               chunk, csize, dtype,
                                               c, n_capos, scale_w,
                                               axis_id, axis_position,
                                               tess_buf, (uint32_t)((uint64_t)MAX_TENSOR + 256*1024));
            fprintf(stderr, "    capo %u/%u: %u blocks, enc_sz=%d\n", c, n_capos, chunk, enc_sz);
            if (enc_sz <= 0) {
                fprintf(stderr, "    FAIL (enc %d)\n", enc_sz);
                skipped_count++;
                continue;
            }

            char path[1024];
            if (n_capos == 1)
                snprintf(path, sizeof(path), "%s/%s.tess", out_dir, basename_of(name));
            else
                snprintf(path, sizeof(path), "%s/%s_capo%u.tess", out_dir, basename_of(name), c);

            FILE *f = fopen(path, "wb");
            if (!f) { printf("    FAIL (write)\n"); skipped_count++; continue; }
            fwrite(tess_buf, 1, (size_t)enc_sz, f);
            fclose(f);

            printf("  -> %s  (%d bytes, capo %u/%u)\n", path, enc_sz, c, n_capos);
            total_encoded += (uint64_t)enc_sz;
            encoded_count++;
        }
    }

    printf("\n  Encoded: %u tensors (%.1f MB)\n  Skipped: %u\n  Done.\n",
           encoded_count, (double)total_encoded / (1024.0*1024.0), skipped_count);

    free(tess_buf);
    gguf_close(&reader);
    return 0;
}
