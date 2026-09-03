/* tess_gguf_pack: GGUF → .tesspack (single file, zero intermediate .tess files)
 * Reads GGUF via gguf_reader.h (mmap), scatter-encodes capos directly into .tesspack.
 * Usage: tess_gguf_pack <input.gguf> <output.tesspack> [tensor_filter]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

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
                           void *dst, uint32_t dst_cap) {
    uint32_t cube_bytes = TESS_TOTAL_SLOTS * cell_size;
    uint32_t payload = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;
    if (dst_cap < payload) return -1;

    uint8_t *p = (uint8_t *)dst;

    TESS_Header hdr;
    tess_header_init(&hdr, gguf_type, cell_size);
    hdr.scale_factor = 65536u;
    hdr.x_slots = TESS_X_SLOTS; hdr.y_slots = TESS_Y_SLOTS; hdr.z_slots = TESS_Z_SLOTS;
    hdr.tensor_count = n_elems;
    memcpy(p, &hdr, TESS_HEADER_SIZE); p += TESS_HEADER_SIZE;

    TESS_Formula fml;
    tess_formula_init(&fml);
    fml.mirror_axis_x = hdr.x_slots; fml.mirror_axis_y = hdr.y_slots; fml.mirror_axis_z = hdr.z_slots;
    fml.stride_seed = TESS_STRIDE_37;
    fml.capo_id = capo_id; fml.capo_total = (uint8_t)capo_total;
    memcpy(p, &fml, TESS_FORMULA_SIZE); p += TESS_FORMULA_SIZE;

    uint8_t *cube = p;
    memset(cube, 0, cube_bytes);
    const uint8_t *src_b = (const uint8_t *)src;
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = tess_stride_scatter(i);
        if (slot >= TESS_TOTAL_SLOTS) slot = i % TESS_TOTAL_SLOTS;
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
        fprintf(stderr, "Usage: %s <input.gguf> <output.tesspack> [tensor_filter]\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *out_path = argv[2];
    const char *filter = (argc > 3) ? argv[3] : NULL;

    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        fprintf(stderr, "Cannot open %s\n", gguf_path);
        return 1;
    }

    fprintf(stderr, "GGUF: %s (%u tensors)\n", gguf_path, gguf.n_tensors);

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { fprintf(stderr, "Cannot create %s\n", out_path); gguf_close(&gguf); return 1; }
    _fseeki64(fout, 64, SEEK_SET); /* skip header (64 bytes) */

    uint32_t capo_buf_cap = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + TESS_TOTAL_SLOTS * GGUF_CELL_SIZE[12] + TESS_CRC_SIZE;
    uint8_t *capo_buf = (uint8_t *)malloc(capo_buf_cap);

    TESSPack_Entry *entries = NULL;
    uint32_t n_entries = 0, entries_cap = 0;
    uint32_t total_capos = 0, filtered = 0;

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (filter && !strstr(gguf.names[i], filter)) continue;
        filtered++;

        uint32_t dtype = gguf.dtypes[i];
        uint32_t csz = (dtype < 16) ? GGUF_CELL_SIZE[dtype] : 0;
        if (csz == 0) continue;

        uint32_t n_blocks = gguf.sizes[i] / csz;
        if (n_blocks == 0) continue;
        uint32_t n_capos = (n_blocks + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;

        const uint8_t *tensor_data = gguf.base + gguf.data_offset + gguf.offsets[i];

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

            int32_t enc_sz = encode_capo(tensor_data + (uint64_t)off * csz,
                                          chunk, csz, dtype, c, n_capos,
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
            total_capos++;
        }
    }

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

    _fseeki64(fout, 0, SEEK_SET);
    uint32_t hdr[16] = {0};
    hdr[0] = TPAK_MAGIC; hdr[1] = 1;
    hdr[2] = total_capos + 1; /* +1 for __gguf_header__ entry */
    hdr[3] = (uint32_t)idx_off;
    fwrite(hdr, 1, 64, fout);
    fclose(fout);

    long fsize = 0;
    fout = fopen(out_path, "rb");
    if (fout) { _fseeki64(fout, 0, SEEK_END); fsize = (long)_ftelli64(fout); fclose(fout); }

    printf("Packed %u tensors, %u capos → %s (%.1f MB)\n",
           filtered, total_capos, out_path, fsize / (1024.0 * 1024.0));

    free(capo_buf); free(entries);
    gguf_close(&gguf);
    return 0;
}
