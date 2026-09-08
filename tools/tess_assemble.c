/* tess_assemble.c — .tess files → reconstruct GGUF (streaming, no OOM)
 *
 * Reads original GGUF for metadata, writes output in two passes:
 *   Pass 1: write header + patch tensor offsets (seekable)
 *   Pass 2: decode + write each tensor sequentially (streaming)
 *
 * No tensor data held in memory simultaneously — O(n) where n = largest single tensor.
 *
 * Usage: tess_assemble <original.gguf> <tess_dir> <output.gguf>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"

static uint32_t gguf_cell_size(uint32_t dtype) {
    static const uint32_t table[] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    if (dtype < sizeof(table)/sizeof(table[0])) return table[dtype];
    if (dtype == 41) return 6;   /* Q1_0 */
    return 0;
}

static const char *basename_of(const char *path) {
    const char *p = strrchr(path, '\\');
    if (!p) p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static int32_t tess_assemble_decode(const void *cube_data, uint32_t cube_bytes,
                                    uint32_t cell_size, uint32_t n_elems,
                                    void *dst, uint32_t dst_cap)
{
    uint32_t out_bytes = n_elems * cell_size;
    if (dst_cap < out_bytes) return -1;
    const uint8_t *cube = (const uint8_t *)cube_data;
    uint8_t *out = (uint8_t *)dst;
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = tess_stride_scatter(i);
        if (slot >= TESS_TOTAL_SLOTS) slot = i % TESS_TOTAL_SLOTS;
        uint32_t src_off = slot * cell_size;
        if (src_off + cell_size <= cube_bytes)
            memcpy(out + (uint64_t)i * cell_size, cube + src_off, cell_size);
    }
    return (int32_t)out_bytes;
}

/* Compute tensor data sizes and total, return data_offset for header end */
static uint64_t compute_tensor_layout(GgufReader *orig, uint32_t *tensor_sizes) {
    static const struct { uint16_t tsz; uint16_t blck; } tinfo[31] = {
        {4,1},{2,1},{18,32},{20,32},{0,0},{0,0},{22,32},{24,32},
        {34,32},{36,32},{84,256},{110,256},{144,256},{176,256},{210,256},{292,256},
        {2,256},{2,256},{2,256},{1,256},{2,32},{1,256},{1,256},{2,256},
        {1,1},{2,1},{4,1},{8,1},{8,1},{1,256},{2,1},
    };
    uint64_t total = 0;
    for (uint64_t i = 0; i < orig->n_tensors; i++) {
        uint8_t dtype = orig->dtypes[i];
        size_t n_elems = 1;
        for (uint8_t d = 0; d < orig->n_dims[i]; d++)
            n_elems *= (size_t)orig->dims[i * 4 + d];
        uint32_t sz = 0;
        if (dtype < 31 && tinfo[dtype].tsz > 0 && tinfo[dtype].blck > 0)
            sz = (uint32_t)((n_elems / tinfo[dtype].blck) * tinfo[dtype].tsz);
        tensor_sizes[i] = sz;
        total += sz;
    }
    return total;
}

/* Write header to f, patching tensor offsets. Returns data section start. */
static uint64_t write_header_patched(FILE *f, GgufReader *orig, const uint32_t *tensor_sizes) {
    /* Byte-copy header */
    fwrite(orig->base, 1, (size_t)orig->data_offset, f);

    /* Walk KV block */
    const uint8_t *p = orig->base + 24;
    const uint8_t *hdr_end = orig->base + orig->data_offset;
    uint64_t nkv;
    memcpy(&nkv, orig->base + 16, 8);
    for (uint64_t k = 0; k < nkv; k++) {
        if (p + 8 > hdr_end) break;
        uint64_t klen; memcpy(&klen, p, 8); p += 8 + klen;
        if (p + 4 > hdr_end) break;
        uint32_t vtype; memcpy(&vtype, p, 4); p += 4;
        switch (vtype) {
        case 0: case 1: case 7: p += 1; break;
        case 2: case 3: p += 2; break;
        case 4: case 5: case 6: p += 4; break;
        case 10: case 11: case 12: p += 8; break;
        case 8: { uint64_t sl; memcpy(&sl, p, 8); p += 8 + sl; } break;
        case 9: {
            uint32_t et; uint64_t na; memcpy(&et, p, 4); memcpy(&na, p+4, 8); p += 12;
            if (et == 8) { for (uint64_t a=0;a<na;a++){uint64_t sl;memcpy(&sl,p,8);p+=8+sl;} }
            else { static const uint8_t esz[]={1,1,2,2,4,4,4,1,0,0,8,8,8}; p+=esz[et<13?et:0]*na; }
        } break;
        }
    }
    uint64_t pos = (uint64_t)(p - orig->base);

    /* Patch tensor offsets */
    uint64_t data_off = 0;
    for (uint64_t i = 0; i < orig->n_tensors; i++) {
        uint64_t nlen; memcpy(&nlen, orig->base+pos, 8); pos += 8 + nlen;
        uint32_t nd; memcpy(&nd, orig->base+pos, 4); pos += 4 + 8*nd;
        pos += 4; /* dtype */
        uint32_t align = (uint32_t)((GGUF_ALIGN - (data_off % GGUF_ALIGN)) % GGUF_ALIGN);
        data_off += align;
        fseek(f, (long)pos, SEEK_SET);
        fwrite(&data_off, 8, 1, f);
        data_off += tensor_sizes[i];
        pos += 8;
    }

    /* Pad to data_offset */
    uint8_t zeros[32] = {0};
    while ((size_t)ftell(f) < orig->data_offset) {
        size_t gap = orig->data_offset - (size_t)ftell(f);
        fwrite(zeros, 1, gap > 32 ? 32 : gap, f);
    }
    return data_off;
}

/* Decode one tensor's capos and stream-write to output file */
static int decode_and_write_tensor(FILE *f, GgufReader *orig, uint32_t idx,
                                    const char *tess_dir, uint32_t data_size)
{
    const char *name = orig->names[idx];
    uint8_t dtype = orig->dtypes[idx];
    uint32_t csize = gguf_cell_size(dtype);
    if (csize == 0) { fprintf(stderr, "  [%3u] %-40s  SKIP (type %u)\n", idx, name, dtype); return 0; }

    uint64_t n_elems = 1;
    for (uint8_t d = 0; d < orig->n_dims[idx]; d++)
        n_elems *= (size_t)orig->dims[idx * 4 + d];
    uint32_t n_blocks = orig->sizes[idx] / csize;
    uint32_t capo_total = (n_blocks + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;

    /* Allocate buffer for ONE capo only (max 144 * csize bytes) */
    size_t capo_buf_max = (size_t)TESS_TOTAL_SLOTS * csize + 256;
    uint8_t *capo_buf = (uint8_t *)malloc(capo_buf_max);
    uint8_t *decode_buf = (uint8_t *)malloc((size_t)TESS_TOTAL_SLOTS * csize);
    if (!capo_buf || !decode_buf) { free(capo_buf); free(decode_buf); return -1; }

    int decoded_blocks = 0;
    for (uint32_t c = 0; c < capo_total; c++) {
        char tess_path[1024];
        if (capo_total == 1)
            snprintf(tess_path, sizeof(tess_path), "%s/%s.tess", tess_dir, basename_of(name));
        else
            snprintf(tess_path, sizeof(tess_path), "%s/%s_capo%u.tess", tess_dir, basename_of(name), c);

        FILE *tf = fopen(tess_path, "rb");
        if (!tf) { fprintf(stderr, "  [%3u] %-40s  MISSING capo %u/%u\n", idx, name, c, capo_total); continue; }
        fseek(tf, 0, SEEK_END);
        long fsz = ftell(tf);
        fseek(tf, 0, SEEK_SET);
        if (fsz > (long)capo_buf_max) { fclose(tf); continue; }
        size_t nread = fread(capo_buf, 1, (size_t)fsz, tf);
        fclose(tf);

        const TESS_Header *hdr = (const TESS_Header *)capo_buf;
        const TESS_Formula *fml = (const TESS_Formula *)(capo_buf + TESS_HEADER_SIZE);
        const uint8_t *cube = capo_buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
        uint32_t cube_bytes = hdr->total_slots * hdr->cell_size;

        /* CRC verify */
        uint64_t stored_crc;
        memcpy(&stored_crc, cube + cube_bytes, TESS_CRC_SIZE);
        if (tess_crc64(cube, cube_bytes) != stored_crc) {
            fprintf(stderr, "  [%3u] %-40s  CRC FAIL capo %u\n", idx, name, c); continue;
        }

        uint32_t elem_count = (n_blocks - fml->capo_id * TESS_TOTAL_SLOTS > TESS_TOTAL_SLOTS)
                              ? TESS_TOTAL_SLOTS : (n_blocks - fml->capo_id * TESS_TOTAL_SLOTS);
        int32_t dec_sz = tess_assemble_decode(cube, cube_bytes, csize, elem_count,
                                               decode_buf, (uint32_t)((uint64_t)elem_count * csize));
        if (dec_sz <= 0) { fprintf(stderr, "  [%3u] %-40s  DECODE FAIL capo %u\n", idx, name, c); continue; }

        fwrite(decode_buf, 1, dec_sz, f);
        decoded_blocks += elem_count;
    }

    free(capo_buf);
    free(decode_buf);

    if (decoded_blocks == 0) {
        fprintf(stderr, "  [%3u] %-40s  ALL CAPOS MISSING\n", idx, name);
        return 0;
    }
    fprintf(stderr, "  [%3u] %-40s  OK (%u blocks, %u/%u capos)\n", idx, name, n_blocks, capo_total, capo_total);
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 4) {
        fprintf(stderr, "Usage: tess_assemble <original.gguf> <tess_dir> <output.gguf>\n");
        return 1;
    }

    GgufReader reader;
    if (gguf_open(argv[1], &reader) != 0) {
        fprintf(stderr, "ERROR: cannot open %s\n", argv[1]);
        return 1;
    }

    fprintf(stderr, "tess_assemble: %s\n", argv[1]);
    fprintf(stderr, "  Tensors: %u\n", reader.n_tensors);

    uint32_t *tensor_sizes = (uint32_t *)calloc(reader.n_tensors, sizeof(uint32_t));
    compute_tensor_layout(&reader, tensor_sizes);

    /* Pass 1: write header */
    FILE *out = fopen(argv[3], "wb");
    if (!out) { fprintf(stderr, "ERROR: cannot create %s\n", argv[3]); return 1; }
    write_header_patched(out, &reader, tensor_sizes);
    fprintf(stderr, "  Header written (%zu bytes)\n", (size_t)ftell(out));

    /* Pass 2: decode + stream-write each tensor */
    uint64_t data_off = 0;
    for (uint64_t i = 0; i < reader.n_tensors; i++) {
        uint32_t a = (uint32_t)((GGUF_ALIGN - (data_off % GGUF_ALIGN)) % GGUF_ALIGN);
        data_off += a;
        /* Pad */
        uint8_t zeros[32] = {0};
        while ((size_t)ftell(out) < reader.data_offset + data_off) {
            size_t gap = (reader.data_offset + data_off) - (size_t)ftell(out);
            fwrite(zeros, 1, gap > 32 ? 32 : gap, out);
        }

        int found = decode_and_write_tensor(out, &reader, (uint32_t)i, argv[2], tensor_sizes[i]);
        if (!found) {
            /* Write zeros for missing tensor */
            uint8_t *z = (uint8_t *)calloc(1, tensor_sizes[i]);
            fwrite(z, 1, tensor_sizes[i], out);
            free(z);
        }
        data_off += tensor_sizes[i];
    }

    fclose(out);
    free(tensor_sizes);
    gguf_close(&reader);

    /* Report */
    FILE *of = fopen(argv[3], "rb");
    if (of) { fseek(of, 0, SEEK_END); fprintf(stderr, "  Output: %s (%ld bytes)\n", argv[3], ftell(of)); fclose(of); }
    fprintf(stderr, "  Done.\n");
    return 0;
}
