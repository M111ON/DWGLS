/* tess_assemble.c — .tess files → reconstruct GGUF
 *
 * Reads original GGUF for metadata (tensor names, shapes, dtypes, KV)
 * and replaces tensor data with decoded .tess payloads.
 * Output is a valid GGUF with all weights restored from .tess containers.
 *
 * Usage: tess_assemble <original.gguf> <tess_dir> <output.gguf>
 *
 * BUILD: gcc -O2 -Wall -Wno-unused-parameter -Icore -o tess_assemble tess_assemble.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

static const char *basename_of(const char *path) {
    const char *p = strrchr(path, '\\');
    if (!p) p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* Inline tess decode — scatter(i) from cube → linear output */
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

/* Write GGUF v3: byte-copy original header + patch data offsets + write decoded data */
static int write_gguf(const char *out_path,
                       const GgufReader *orig,
                       uint8_t **decoded_data)
{
    FILE *f = fopen(out_path, "wb");
    if (!f) return -1;

    uint64_t n_tensors = orig->n_tensors;

    /* 1) Byte-copy entire original header [0, data_offset) — preserves KV + tensor_info exactly */
    fwrite(orig->base, 1, (size_t)orig->data_offset, f);

    /* 2) Compute new data sizes for decoded tensors */
    static const struct { uint16_t tsz; uint16_t blck; } tinfo[31] = {
        {4,1},{2,1},{18,32},{20,32},{0,0},{0,0},{22,32},{24,32},
        {34,32},{36,32},{84,256},{110,256},{144,256},{176,256},{210,256},{292,256},
        {2,256},{2,256},{2,256},{1,256},{2,32},{1,256},{1,256},{2,256},
        {1,1},{2,1},{4,1},{8,1},{8,1},{1,256},{2,1},
    };

    uint32_t *tensor_sizes = (uint32_t *)calloc(n_tensors, sizeof(uint32_t));
    uint64_t total_data = 0;
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint8_t dtype = orig->dtypes[i];
        size_t n_elems = 1;
        for (uint8_t d = 0; d < orig->n_dims[i]; d++)
            n_elems *= (size_t)orig->dims[i * 4 + d];

        uint32_t data_size = 0;
        if (dtype < 31 && tinfo[dtype].tsz > 0 && tinfo[dtype].blck > 0)
            data_size = (uint32_t)((n_elems / tinfo[dtype].blck) * tinfo[dtype].tsz);

        tensor_sizes[i] = data_size;
        total_data += data_size;
    }

    /* 3) Patch data offsets in tensor_info: each tensor gets sequential offset aligned to 32 */
    {
        /* The original's tensor_info entries end with an 8-byte data_offset field.
         * We need to find each one and overwrite it. The entries are sequential after KV.
         * Each entry: name_len(8) + name + n_dims(4) + dims(8*n_dims) + dtype(4) + data_offset(8)
         * We can compute positions by walking the header. */
        uint64_t pos = 24; /* skip magic + version + n_tensors + n_kv */

        /* Skip KV block — scan using same logic as gguf_reader */
        {
            const uint8_t *p = orig->base + 24;
            const uint8_t *hdr_end = orig->base + orig->data_offset;
            uint64_t nkv;
            memcpy(&nkv, orig->base + 16, 8);
            for (uint64_t k = 0; k < nkv; k++) {
                if (p + 8 > hdr_end) break;
                uint64_t klen;
                memcpy(&klen, p, 8);
                p += 8 + klen;
                if (p + 4 > hdr_end) break;
                uint32_t vtype;
                memcpy(&vtype, p, 4);
                p += 4;
                /* Skip value — use gguf_value_skip helper or inline */
                switch (vtype) {
                case 0: case 1: case 7: p += 1; break;
                case 2: case 3: p += 2; break;
                case 4: case 5: case 6: p += 4; break;
                case 10: case 11: case 12: p += 8; break;
                case 8: { /* STRING */
                    uint64_t slen;
                    memcpy(&slen, p, 8);
                    p += 8 + slen;
                } break;
                case 9: { /* ARRAY */
                    uint32_t etype;
                    uint64_t narr;
                    memcpy(&etype, p, 4);
                    memcpy(&narr, p + 4, 8);
                    p += 12;
                    if (etype == 8) {
                        for (uint64_t a = 0; a < narr; a++) {
                            uint64_t sl;
                            memcpy(&sl, p, 8);
                            p += 8 + sl;
                        }
                    } else {
                        static const uint8_t esz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
                        p += esz[etype < 13 ? etype : 0] * narr;
                    }
                } break;
                }
            }
            pos = (uint64_t)(p - orig->base);
        }

        /* Now pos points to start of tensor_info. Walk and patch data_offset. */
        uint64_t data_off = 0;
        for (uint64_t i = 0; i < n_tensors; i++) {
            /* Skip name_len(8) + name */
            uint64_t nlen;
            memcpy(&nlen, orig->base + pos, 8);
            pos += 8 + nlen;
            /* Skip n_dims(4) + dims(8*n_dims) */
            uint32_t nd;
            memcpy(&nd, orig->base + pos, 4);
            pos += 4 + 8 * nd;
            /* Skip dtype(4) */
            pos += 4;
            /* Patch data_offset(8) */
            /* Align to 32 bytes */
            uint32_t align = (uint32_t)((GGUF_ALIGN - (data_off % GGUF_ALIGN)) % GGUF_ALIGN);
            data_off += align;
            fseek(f, (long)pos, SEEK_SET);
            fwrite(&data_off, 8, 1, f);
            data_off += tensor_sizes[i];
            pos += 8;
        }
    }

    /* 4) Pad to data_offset */
    {
        uint8_t zeros[32] = {0};
        while ((size_t)ftell(f) < orig->data_offset) {
            size_t gap = orig->data_offset - (size_t)ftell(f);
            fwrite(zeros, 1, gap > 32 ? 32 : gap, f);
        }
    }

    /* 5) Write tensor data — aligned sequentially */
    uint64_t data_off = 0;
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint32_t a = (uint32_t)((GGUF_ALIGN - (data_off % GGUF_ALIGN)) % GGUF_ALIGN);
        data_off += a;
        /* Pad */
        {
            uint8_t zeros[32] = {0};
            while ((size_t)ftell(f) < orig->data_offset + data_off) {
                size_t gap = (orig->data_offset + data_off) - (size_t)ftell(f);
                fwrite(zeros, 1, gap > 32 ? 32 : gap, f);
            }
        }
        if (decoded_data[i]) {
            fwrite(decoded_data[i], 1, tensor_sizes[i], f);
        } else {
            /* No .tess — write zeros */
            uint8_t *zeros = (uint8_t *)calloc(1, tensor_sizes[i]);
            fwrite(zeros, 1, tensor_sizes[i], f);
            free(zeros);
        }
        data_off += tensor_sizes[i];
    }

    free(tensor_sizes);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 4) {
        fprintf(stderr, "Usage: tess_assemble <original.gguf> <tess_dir> <output.gguf>\n");
        fprintf(stderr, "  Reads original GGUF for metadata, replaces tensor data with .tess decoded payloads.\n");
        return 1;
    }

    const char *gguf_path = argv[1];
    const char *tess_dir = argv[2];
    const char *out_path = argv[3];

    /* Open original GGUF */
    GgufReader reader;
    if (gguf_open(gguf_path, &reader) != 0) {
        fprintf(stderr, "ERROR: cannot open %s\n", gguf_path);
        return 1;
    }

    fprintf(stderr, "tess_assemble: %s\n", gguf_path);
    fprintf(stderr, "  Tensors: %u\n", reader.n_tensors);
    fprintf(stderr, "  Tess dir: %s\n", tess_dir);

    /* Decode each .tess file */
    uint8_t **decoded = (uint8_t **)calloc(reader.n_tensors, sizeof(uint8_t *));
    uint32_t found = 0, missing = 0;

    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        const char *name = reader.names[i];
        uint8_t dtype = reader.dtypes[i];
        uint32_t tsize = reader.sizes[i];

        uint32_t csize = (dtype < 16) ? GGUF_CELL_SIZE[dtype] : 0;
        if (csize == 0) {
            fprintf(stderr, "  [%3u] %-40s  SKIP (type %u)\n", i, name, dtype);
            missing++;
            continue;
        }

        /* Compute element count for this tensor */
        uint64_t n_elems = 1;
        for (uint8_t d = 0; d < reader.n_dims[i]; d++)
            n_elems *= reader.dims[i * 4 + d];
        uint32_t n_blocks = tsize / csize;
        uint32_t capo_total_needed = (n_blocks + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;
        decoded[i] = (uint8_t *)calloc((uint64_t)n_blocks, csize);

        int decoded_blocks = 0;
        for (uint32_t c = 0; c < capo_total_needed; c++) {
            char tess_path[1024];
            if (capo_total_needed == 1)
                snprintf(tess_path, sizeof(tess_path), "%s/%s.tess", tess_dir, basename_of(name));
            else
                snprintf(tess_path, sizeof(tess_path), "%s/%s_capo%u.tess", tess_dir, basename_of(name), c);

            FILE *tf = fopen(tess_path, "rb");
            if (!tf) {
                fprintf(stderr, "  [%3u] %-40s  MISSING capo %u/%u\n", i, name, c, capo_total_needed);
                continue;
            }

            fseek(tf, 0, SEEK_END);
            long fsz = ftell(tf);
            fseek(tf, 0, SEEK_SET);
            uint8_t *buf = (uint8_t *)malloc((size_t)fsz);
            fread(buf, 1, (size_t)fsz, tf);
            fclose(tf);

            const TESS_Header *hdr = (const TESS_Header *)buf;
            const TESS_Formula *fml = (const TESS_Formula *)(buf + TESS_HEADER_SIZE);
            const uint8_t *cube = buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
            uint32_t cube_bytes = hdr->total_slots * hdr->cell_size;

            /* Verify CRC-64 */
            uint64_t stored_crc, computed_crc;
            memcpy(&stored_crc, cube + cube_bytes, TESS_CRC_SIZE);
            computed_crc = tess_crc64(cube, cube_bytes);
            if (computed_crc != stored_crc) {
                fprintf(stderr, "  [%3u] %-40s  CRC FAIL capo %u\n", i, name, c);
                free(buf); continue;
            }

            uint32_t capo_id = fml->capo_id;
            uint32_t elem_count = (n_blocks - capo_id * TESS_TOTAL_SLOTS > TESS_TOTAL_SLOTS)
                                  ? TESS_TOTAL_SLOTS : (n_blocks - capo_id * TESS_TOTAL_SLOTS);
            uint8_t *dst = decoded[i] + (uint64_t)capo_id * TESS_TOTAL_SLOTS * csize;
            int32_t dec_sz = tess_assemble_decode(cube, cube_bytes, csize, elem_count,
                                                   dst, (uint32_t)((uint64_t)elem_count * csize));
            free(buf);

            if (dec_sz <= 0) {
                fprintf(stderr, "  [%3u] %-40s  DECODE FAIL capo %u\n", i, name, c);
                continue;
            }
            decoded_blocks += elem_count;
        }

        if (decoded_blocks == 0) {
            fprintf(stderr, "  [%3u] %-40s  ALL CAPOS MISSING\n", i, name);
            free(decoded[i]); decoded[i] = NULL; missing++;
            continue;
        }

        fprintf(stderr, "  [%3u] %-40s  OK (%u blocks, %u/%u decoded)\n",
                i, name, n_blocks, decoded_blocks, capo_total_needed);
        found++;
    }

    fprintf(stderr, "\n  Found: %u  Missing: %u\n", found, missing);

    /* Write assembled GGUF */
    fprintf(stderr, "  Writing %s ...\n", out_path);
    int rc = write_gguf(out_path, &reader, decoded);
    if (rc != 0) {
        fprintf(stderr, "ERROR: write failed\n");
    } else {
        /* Verify output file size */
        FILE *of = fopen(out_path, "rb");
        if (of) {
            fseek(of, 0, SEEK_END);
            long out_sz = ftell(of);
            fclose(of);
            fprintf(stderr, "  Output: %s (%ld bytes)\n", out_path, out_sz);
            fprintf(stderr, "  Done.\n");
        }
    }

    /* Cleanup */
    for (uint32_t i = 0; i < reader.n_tensors; i++)
        if (decoded[i]) free(decoded[i]);
    free(decoded);
    gguf_close(&reader);
    return rc;
}
