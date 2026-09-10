/* tess_gguf_pack: GGUF → .tesspack (single file, zero intermediate .tess files)
 * Reads GGUF via gguf_reader.h (mmap), scatter-encodes capos directly into .tesspack.
 * Usage: tess_gguf_pack <input.gguf> <output.tesspack> [tensor_filter]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gguf_reader.h"
#include "ggml.h"
#include "geo_tess_container.h"

static uint32_t gguf_cell_size(uint32_t dtype) {
    static const uint32_t table[] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    if (dtype < sizeof(table)/sizeof(table[0])) return table[dtype];
    if (dtype == 41) return 6;   /* Q1_0 */
    return 0;
}

#define TPAK_MAGIC 0x5450414Bu

#if !defined(_WIN32) && !defined(_ftelli64)
#define _ftelli64 ftello
#define _fseeki64 fseeko
#endif

/* Index entry: matches tess_packer.c format exactly */
#pragma pack(push, 1)
typedef struct {
    uint8_t  name_len;
    char     name[255];
    uint32_t capo_id;
    uint64_t file_offset;
    uint32_t capo_size;
} TESSPack_Entry;
#pragma pack(pop)

static int32_t encode_capo(const void *src, uint32_t n_elems, uint32_t cell_size,
                           uint32_t gguf_type, uint32_t capo_id, uint32_t capo_total,
                           uint32_t scale_w, uint32_t axis_id, uint32_t axis_position,
                           void *dst, uint32_t dst_cap) {
    uint8_t *p = (uint8_t *)dst;

    TESS_Header hdr;
    tess_header_init(&hdr, gguf_type, cell_size);
    hdr.scale_factor = 65536u;
    if (scale_w > 0) {
        double s = exp2(-(double)scale_w / 12.0);
        hdr.scale_factor = (uint32_t)(s * 65536.0 + 0.5);
    }
    uint32_t eff_slots = tess_effective_slots(&hdr);
    uint32_t cube_bytes = eff_slots * cell_size;
    uint32_t payload = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;
    if (dst_cap < payload) return -1;
    hdr.x_slots = TESS_X_SLOTS; hdr.y_slots = TESS_Y_SLOTS; hdr.z_slots = TESS_Z_SLOTS;
    hdr.tensor_count = n_elems;
    memcpy(p, &hdr, TESS_HEADER_SIZE); p += TESS_HEADER_SIZE;

    TESS_Formula fml;
    tess_formula_init(&fml);
    fml.mirror_axis_x = hdr.x_slots; fml.mirror_axis_y = hdr.y_slots; fml.mirror_axis_z = hdr.z_slots;
    fml.stride_seed = TESS_STRIDE_37;
    fml.capo_id = capo_id; fml.capo_total = (uint8_t)capo_total;
    fml.axis_id = (uint8_t)axis_id;
    fml.axis_position = axis_position;
    memcpy(p, &fml, TESS_FORMULA_SIZE); p += TESS_FORMULA_SIZE;

    uint8_t *cube = p;
    memset(cube, 0, cube_bytes);
    const uint8_t *src_b = (const uint8_t *)src;
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = tess_stride_scatter_in(i, eff_slots);
        uint32_t off = slot * cell_size;
        if (off + cell_size <= cube_bytes)
            memcpy(cube + off, src_b + (uint64_t)i * cell_size, cell_size);
    }
    p += cube_bytes;

    uint64_t crc = tess_crc64(cube, cube_bytes);
    memcpy(p, &crc, TESS_CRC_SIZE);
    ((TESS_Header *)dst)->cube_checksum = crc;
    return (int32_t)(p + TESS_CRC_SIZE - (uint8_t *)dst);
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.gguf> <output.tesspack> [tensor_filter] [--scale W] [--axis N] [--position P]\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *out_path = argv[2];
    const char *filter = NULL;
    uint32_t scale_w = 0;
    uint32_t axis_id = 0;       /* default: X axis */
    uint32_t axis_position = 0;  /* default: position 0 */

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
        } else if (!filter) {
            filter = argv[a];
        }
    }
    fprintf(stderr, "scale W=%u (s=%.4f), axis=%u, position=%u\n",
            scale_w, exp2(-(double)scale_w / 12.0), axis_id, axis_position);

    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        fprintf(stderr, "Cannot open %s\n", gguf_path);
        return 1;
    }

    fprintf(stderr, "GGUF: %s (%u tensors)\n", gguf_path, gguf.n_tensors);

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { fprintf(stderr, "Cannot create %s\n", out_path); gguf_close(&gguf); return 1; }
    _fseeki64(fout, 64, SEEK_SET); /* skip header (64 bytes) */

    uint32_t capo_buf_cap = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + TESS_TOTAL_SLOTS * gguf_cell_size(12) + TESS_CRC_SIZE;
    uint8_t *capo_buf = (uint8_t *)malloc(capo_buf_cap);

    TESSPack_Entry *entries = NULL;
    uint32_t n_entries = 0, entries_cap = 0;
    uint32_t total_capos = 0, filtered = 0;

    uint32_t n_onion = 0;
    uint64_t onion_data_start = 0, onion_data_end = 0;
    uint64_t pack_sig64 = 0;  /* running XOR-fold integrity for all capo data */

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (filter && !strstr(gguf.names[i], filter)) continue;
        filtered++;

        uint32_t dtype = gguf.dtypes[i];
        uint32_t csz = gguf_cell_size(dtype);
        if (csz == 0) continue;

        uint32_t n_blocks = gguf.sizes[i] / csz;
        if (n_blocks == 0) continue;
        uint32_t n_capos = (n_blocks + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;

        const uint8_t *tensor_data = gguf.base + gguf.data_offset + gguf.offsets[i];

        /* ── ONION path: f32/f16 → convert to f16 contiguous blob (no scatter) ── */
        int is_onion = (dtype == 0 || dtype == 1);
        if (is_onion) {
            uint32_t n_elems = gguf.sizes[i] / (dtype == 0 ? 4 : 2);
            fprintf(stderr, "  [%3u] %-40s  ONION  dtype=%u  elems=%u  %u bytes\n",
                    i, gguf.names[i], dtype, n_elems, gguf.sizes[i]);

            if (n_onion == 0)
                onion_data_start = (uint64_t)_ftelli64(fout);

            /* Store raw bytes (f32 or f16, no conversion) for bitwise lossless */
            fwrite(tensor_data, 1, gguf.sizes[i], fout);
            onion_data_end = (uint64_t)_ftelli64(fout);

            /* sig32: XOR-fold raw tensor data */
            for (uint64_t b = 0; b + 7 < gguf.sizes[i]; b += 8) {
                uint64_t v;
                memcpy(&v, tensor_data + b, 8);
                pack_sig64 ^= v;
            }

            if (n_entries >= entries_cap) {
                entries_cap = entries_cap ? entries_cap * 2 : 4096;
                entries = realloc(entries, entries_cap * sizeof(TESSPack_Entry));
            }
            TESSPack_Entry *e = &entries[n_entries++];
            memset(e, 0, sizeof(*e));
            e->name_len = (uint8_t)strlen(gguf.names[i]);
            memcpy(e->name, gguf.names[i], e->name_len);
            e->capo_id = 0xFFFFFFFFu;  /* sentinel: onion entry */
            e->file_offset = onion_data_end - (uint64_t)gguf.sizes[i];
            e->capo_size = gguf.sizes[i]; /* raw bytes */
            n_onion++;
            continue;
        }

        fprintf(stderr, "  [%3u] %-40s  cell=%3u  blocks=%7u  capos=%u\n",
                i, gguf.names[i], csz, n_blocks, n_capos);

        for (uint32_t c = 0; c < n_capos; c++) {
            uint32_t off = c * TESS_TOTAL_SLOTS;
            uint32_t chunk = n_blocks - off;
            if (chunk > TESS_TOTAL_SLOTS) chunk = TESS_TOTAL_SLOTS;

            uint32_t cube_bytes = TESS_TOTAL_SLOTS * csz;
            uint32_t total_sz = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;
            if (total_sz > capo_buf_cap) {
                capo_buf_cap = total_sz;
                capo_buf = realloc(capo_buf, capo_buf_cap);
            }

            int32_t enc_sz = encode_capo(tensor_data + (uint64_t)off * csz, chunk, csz,
                         dtype, c, n_capos, scale_w, axis_id, axis_position,
                         capo_buf, capo_buf_cap);
            if (enc_sz <= 0) { fprintf(stderr, "    FAIL capo %u\n", c); continue; }

            if (n_entries >= entries_cap) {
                entries_cap = entries_cap ? entries_cap * 2 : 4096;
                entries = realloc(entries, entries_cap * sizeof(TESSPack_Entry));
            }
            TESSPack_Entry *e = &entries[n_entries++];
            memset(e, 0, sizeof(*e));
            e->name_len = (uint8_t)strlen(gguf.names[i]);
            memcpy(e->name, gguf.names[i], e->name_len);
            e->capo_id = c;
            e->file_offset = (uint64_t)_ftelli64(fout);
            e->capo_size = (uint32_t)enc_sz;

            fwrite(capo_buf, 1, (size_t)enc_sz, fout);

            /* sig32: XOR-fold encoded capo bytes */
            for (uint32_t b = 0; b + 7 < (uint32_t)enc_sz; b += 8) {
                uint64_t v;
                memcpy(&v, capo_buf + b, 8);
                pack_sig64 ^= v;
            }

            total_capos++;
        }
    }
    fprintf(stderr, "[pack] all %u tensors encoded, %u capos total\n", gguf.n_tensors, total_capos);

    /* ── embed GGUF header (0..data_offset) as reserved index entry ── */
    uint64_t gguf_hdr_off = (uint64_t)_ftelli64(fout);
    uint32_t gguf_hdr_sz = (uint32_t)gguf.data_offset;
    fwrite(gguf.base, 1, gguf_hdr_sz, fout);

    TESSPack_Entry gguf_hdr_entry;
    memset(&gguf_hdr_entry, 0, sizeof(gguf_hdr_entry));
    gguf_hdr_entry.name_len = (uint8_t)strlen(TPAK_GGUF_HEADER_NAME);
    memcpy(gguf_hdr_entry.name, TPAK_GGUF_HEADER_NAME, gguf_hdr_entry.name_len);
    gguf_hdr_entry.capo_id = 0;
    gguf_hdr_entry.file_offset = gguf_hdr_off;
    gguf_hdr_entry.capo_size = gguf_hdr_sz;

    uint64_t idx_off = (uint64_t)_ftelli64(fout);
    for (uint32_t i = 0; i < n_entries; i++) {
        fwrite(&entries[i].name_len, 1, 1, fout);
        fwrite(entries[i].name, 1, entries[i].name_len, fout);
        fwrite(&entries[i].capo_id, 4, 1, fout);
        fwrite(&entries[i].file_offset, 8, 1, fout);
        fwrite(&entries[i].capo_size, 4, 1, fout);
    }
    /* write GGUF header entry last (after tensor capo entries) */
    fwrite(&gguf_hdr_entry.name_len, 1, 1, fout);
    fwrite(gguf_hdr_entry.name, 1, gguf_hdr_entry.name_len, fout);
    fwrite(&gguf_hdr_entry.capo_id, 4, 1, fout);
    fwrite(&gguf_hdr_entry.file_offset, 8, 1, fout);
    fwrite(&gguf_hdr_entry.capo_size, 4, 1, fout);

    /* ── auto-detect residual redirects (weight-tying + internal tensors) ── */
    typedef struct {
        uint8_t  name_len;
        char     name[255];
        uint8_t  src_len;
        char     src[255];
        uint8_t  src_type;
        uint8_t  dst_type;
        uint8_t  transform;
        uint8_t  _pad;
    } ResidEntry;  /* 522 bytes, matches TESS_ResidualEntry */

    ResidEntry residuals[32];
    uint32_t n_residual = 0;

    /* check for weight-tying: output.weight absent + token_embd.weight present */
    int has_output_weight = 0, has_token_embd = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (strcmp(gguf.names[i], "output.weight") == 0) has_output_weight = 1;
        if (strcmp(gguf.names[i], "token_embd.weight") == 0) has_token_embd = 1;
    }
    if (!has_output_weight && has_token_embd && n_residual < 32) {
        ResidEntry *re = &residuals[n_residual++];
        memset(re, 0, sizeof(*re));
        re->name_len = 13; memcpy(re->name, "output.weight", 13);
        re->src_len  = 18; memcpy(re->src,  "token_embd.weight", 18);
        re->src_type = 8;   /* Q8_0 */
        re->dst_type = 0;   /* F32 */
        re->transform = 1;  /* TYPE_CAST */
        fprintf(stderr, "  [auto] RESIDUAL: output.weight → token_embd.weight (TYPE_CAST Q8_0→F32)\n");
    }

    /* write residual entries after index */
    uint64_t residual_off = 0;
    if (n_residual > 0) {
        residual_off = (uint64_t)_ftelli64(fout);
        for (uint32_t i = 0; i < n_residual; i++)
            fwrite(&residuals[i], 1, sizeof(ResidEntry), fout);
    }

    /* ── write scale log (if W != 0) ── */
    uint64_t scale_log_off = 0;
    uint32_t n_scale_log = 0;
    typedef struct { uint32_t capo_id; uint16_t old_w; uint16_t new_w; } ScaleLogEntry;
    ScaleLogEntry slog;

    if (scale_w != 0) {
        scale_log_off = (uint64_t)_ftelli64(fout);
        slog.capo_id = 0xFFFFFFFFu;  /* pack-wide event */
        slog.old_w   = 0;
        slog.new_w   = (uint16_t)scale_w;
        fwrite(&slog, 1, sizeof(slog), fout);
        n_scale_log = 1;
    }

    /* ── write header (version 3 if scale log, version 2 if residual) ── */
    fflush(fout);
    fclose(fout);

    fout = fopen(out_path, "r+b");  /* reopen for in-place header write */
    if (!fout) { fprintf(stderr, "Cannot reopen %s for header\n", out_path); return 1; }
    _fseeki64(fout, 0, SEEK_SET);
    uint32_t hdr[16] = {0};
    hdr[0] = TPAK_MAGIC;
    hdr[1] = n_scale_log > 0 ? 3 : (n_residual > 0 ? 2 : 1);  /* version 3 if scale log */
    hdr[2] = n_entries + 1;  /* +1 for __gguf_header__ entry */
    hdr[3] = (uint32_t)idx_off;
    hdr[4] = (uint32_t)(onion_data_start ? onion_data_start : 0);
    hdr[5] = (uint32_t)(onion_data_end - onion_data_start);
    hdr[6] = (uint32_t)residual_off;
    hdr[7] = n_residual;
    hdr[8] = (uint32_t)((pack_sig64 >> 32) ^ (pack_sig64 & 0xFFFFFFFF));  /* sig32 integrity */
    hdr[9] = (uint32_t)scale_log_off;
    hdr[10] = n_scale_log;
    fwrite(hdr, 1, 64, fout);
    fclose(fout);

    long fsize = 0;
    fout = fopen(out_path, "rb");
    if (fout) { _fseeki64(fout, 0, SEEK_END); fsize = (long)_ftelli64(fout); fclose(fout); }

    printf("Packed %u tensors, %u capos, %u onion → %s (%.1f MB) sig32=0x%08X\n",
           filtered, total_capos, n_onion, out_path, fsize / (1024.0 * 1024.0),
           hdr[8]);

    free(capo_buf); free(entries);
    gguf_close(&gguf);
    return 0;
}
