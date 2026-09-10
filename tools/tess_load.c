/* tess_load.c — .tess → raw weights decode tool
 *
 * Reads .tess files and decodes stride-37 scattered CubeData back to
 * linear weight ordering. Verifies CRC-64 integrity.
 * Supports multi-cube capo files (large tensors split across capos).
 *
 * Usage:
 *   tess_load <file.tess> [output.bin]       — decode to file
 *   tess_load <file.tess> --dram              — decode into DRamTile slots
 *   tess_load <file.tess>                     — print header info only
 *
 * For multi-cube: pass the capo-0 file; remaining capos auto-discovered
 *
 * BUILD: gcc -O2 -Wall -Wno-unused-parameter -Icore -Icore/infra -o tess_load tess_load.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "geo_tess_container.h"
#include "dramtile_store.h"
#include "tess_scatter_gpu.h"

static const char *type_name(uint32_t dtype) {
    switch (dtype) {
        case 0:  return "F32";
        case 1:  return "F16";
        case 2:  return "Q4_0";
        case 3:  return "Q4_1";
        case 6:  return "Q5_0";
        case 7:  return "Q5_1";
        case 8:  return "Q8_0";
        case 9:  return "Q8_1";
        case 12: return "Q4_K";
        case 13: return "Q5_K";
        case 14: return "Q6_K";
        case 15: return "Q8_K";
        case 30: return "BF16";
        default: return "???";
    }
}

/* Inline tess decode — scatter(i) from cube → linear output */
static int32_t tess_load_decode(const void *cube_data_in, uint32_t cube_bytes,
                                 uint32_t cell_size, uint32_t n_elems,
                                 void *dst, uint32_t dst_cap)
{
    uint32_t out_bytes = n_elems * cell_size;
    if (dst_cap < out_bytes) return -1;

    const uint8_t *cube = (const uint8_t *)cube_data_in;
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

/* Derive capo file path from the capo-0 path.
 * If path ends with "_capo0.tess" → strip _capo0, add _capoN.tess.
 * If path ends with ".tess" (single-cube) → insert "_capoN" before ".tess".
 */
static void capo_path(char *dst, size_t cap, const char *base_path, uint32_t c) {
    size_t len = strlen(base_path);
    if (len > 11 && strcmp(base_path + len - 11, "_capo0.tess") == 0) {
        /* Strip "_capo0.tess" → "_capoN.tess" */
        size_t prefix = len - 11;
        snprintf(dst, cap, "%.*s_capo%u.tess", (int)prefix, base_path, c);
    } else if (len > 5 && strcmp(base_path + len - 5, ".tess") == 0) {
        /* Insert _capoN before .tess */
        size_t prefix = len - 5;
        snprintf(dst, cap, "%.*s_capo%u.tess", (int)prefix, base_path, c);
    } else {
        snprintf(dst, cap, "%s_capo%u", base_path, c);
    }
}

/* ── DRamTile integration: decode .tess into DRamTile slot region ──
 * Stores each decoded capo as a named tensor in DRamTile.
 * Returns DRamTile address for the decoded data (for gguf_box routing).
 * Uses tess_scatter_gpu.h for dispatch (GPU when available).
 */
static uint32_t tess_load_to_dram(const char *tess_path, DRamTileStore *store) {
    FILE *f = fopen(tess_path, "rb");
    if (!f) { fprintf(stderr, "ERROR: cannot open %s\n", tess_path); return 0; }

    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_sz < (long)(TESS_HEADER_SIZE + TESS_FORMULA_SIZE + TESS_CRC_SIZE)) {
        fprintf(stderr, "ERROR: file too small\n"); fclose(f); return 0;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)file_sz);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)file_sz, f) != (size_t)file_sz) {
        free(buf); fclose(f); return 0;
    }
    fclose(f);

    const TESS_Header *hdr = (const TESS_Header *)buf;
    const TESS_Formula *fml = (const TESS_Formula *)(buf + TESS_HEADER_SIZE);
    if (tess_header_validate(hdr) != 0) {
        fprintf(stderr, "ERROR: bad header\n"); free(buf); return 0;
    }

    uint32_t cell_sz = hdr->cell_size;
    uint32_t cube_bytes = hdr->total_slots * cell_sz;
    const uint8_t *cube_data = buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
    uint32_t capo_total = fml->capo_total ? fml->capo_total : 1;
    uint32_t n_elems_per_capo = hdr->tensor_count ? hdr->tensor_count : hdr->total_slots;

    /* Total decoded size */
    uint64_t total_elems = (uint64_t)n_elems_per_capo * capo_total;
    uint64_t total_bytes = total_elems * cell_sz;

    /* Allocate decode buffer */
    uint8_t *out = (uint8_t *)malloc((size_t)total_bytes);
    if (!out) { free(buf); return 0; }
    memset(out, 0, (size_t)total_bytes);

    /* Decode all capos (same logic as file output path) */
    uint32_t dec_sz = tess_load_decode(cube_data, cube_bytes, cell_sz, n_elems_per_capo,
                                       out, (uint32_t)(n_elems_per_capo * cell_sz));
    if (dec_sz <= 0) {
        fprintf(stderr, "ERROR: capo 0 decode failed\n"); free(out); free(buf); return 0;
    }
    uint64_t total_decoded = n_elems_per_capo;

    for (uint32_t c = 1; c < capo_total; c++) {
        char capo_file[1024];
        capo_path(capo_file, sizeof(capo_file), tess_path, c);
        FILE *cf = fopen(capo_file, "rb");
        if (!cf) continue;

        fseek(cf, 0, SEEK_END);
        long capo_sz = ftell(cf);
        fseek(cf, 0, SEEK_SET);

        uint8_t *capo_buf = (uint8_t *)malloc((size_t)capo_sz);
        if (!capo_buf) { fclose(cf); continue; }
        if (fread(capo_buf, 1, (size_t)capo_sz, cf) != (size_t)capo_sz) {
            free(capo_buf); fclose(cf); continue;
        }
        fclose(cf);

        const TESS_Header *chdr = (const TESS_Header *)capo_buf;
        const uint8_t *ccube = capo_buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
        uint32_t ccube_bytes = chdr->total_slots * chdr->cell_size;

        uint64_t cc_stored, cc_computed;
        memcpy(&cc_stored, ccube + ccube_bytes, TESS_CRC_SIZE);
        cc_computed = tess_crc64(ccube, ccube_bytes);
        if (cc_computed != cc_stored) { free(capo_buf); continue; }

        uint32_t ce = chdr->tensor_count ? chdr->tensor_count : chdr->total_slots;
        uint8_t *dst = out + total_decoded * cell_sz;
        dec_sz = tess_load_decode(ccube, ccube_bytes, chdr->cell_size, ce,
                                   dst, (uint32_t)(ce * chdr->cell_size));
        free(capo_buf);
        if (dec_sz > 0) total_decoded += ce;
    }

    /* Store in DRamTile: tensor name derived from file path */
    const char *basename = tess_path;
    for (const char *p = tess_path; *p; p++) {
        if (*p == '/' || *p == '\\') basename = p + 1;
    }
    /* strip .tess extension */
    char tensor_name[256];
    strncpy(tensor_name, basename, 255);
    tensor_name[255] = '\0';
    size_t nlen = strlen(tensor_name);
    if (nlen > 5 && strcmp(tensor_name + nlen - 5, ".tess") == 0)
        tensor_name[nlen - 5] = '\0';

    uint8_t *dram_ptr = dt_put(store, tensor_name, out, (size_t)total_decoded * cell_sz);
    uint32_t dram_addr = dram_ptr ? dt_name_to_addr(tensor_name) : 0;

    printf("  DRamTile: tensor='%s' addr=%u decoded=%lu bytes ptr=%p\n",
           tensor_name, dram_addr, (unsigned long)(total_decoded * cell_sz),
           (void *)dram_ptr);

    free(out);
    free(buf);
    return dram_addr;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: tess_load <file.tess> [output.bin]\n");
        fprintf(stderr, "       tess_load <file.tess> --dram\n");
        fprintf(stderr, "  For multi-cube tensors, pass the capo-0 file.\n");
        fprintf(stderr, "  --dram: decode into DRamTile slot region (zero-copy)\n");
        return 1;
    }

    const char *tess_path = argv[1];
    const char *out_path = NULL;
    int use_dram = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--dram") == 0) use_dram = 1;
        else out_path = argv[i];
    }

    /* Load capo-0 file */
    FILE *f = fopen(tess_path, "rb");
    if (!f) { fprintf(stderr, "ERROR: cannot open %s\n", tess_path); return 1; }

    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_sz < (long)(TESS_HEADER_SIZE + TESS_FORMULA_SIZE + TESS_CRC_SIZE)) {
        fprintf(stderr, "ERROR: file too small\n"); fclose(f); return 1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)file_sz);
    if (!buf) { fprintf(stderr, "ERROR: alloc\n"); fclose(f); return 1; }
    if (fread(buf, 1, (size_t)file_sz, f) != (size_t)file_sz) {
        fprintf(stderr, "ERROR: read failed\n"); free(buf); fclose(f); return 1;
    }
    fclose(f);

    const TESS_Header *hdr = (const TESS_Header *)buf;
    const TESS_Formula *fml = (const TESS_Formula *)(buf + TESS_HEADER_SIZE);
    if (tess_header_validate(hdr) != 0) {
        fprintf(stderr, "ERROR: bad header\n"); free(buf); return 1;
    }

    uint32_t cell_sz = hdr->cell_size;
    uint32_t cube_bytes = hdr->total_slots * cell_sz;
    const uint8_t *cube_data = buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
    uint64_t stored_crc, computed_crc;
    memcpy(&stored_crc, cube_data + cube_bytes, TESS_CRC_SIZE);
    computed_crc = tess_crc64(cube_data, cube_bytes);

    uint32_t capo_total = fml->capo_total ? fml->capo_total : 1;
    uint32_t n_elems_per_capo = hdr->tensor_count ? hdr->tensor_count : hdr->total_slots;

    printf("tess_load: %s\n", tess_path);
    printf("  Magic:        0x%08X %s\n", hdr->magic,
           hdr->magic == GEO_TESS_MAGIC ? "(valid)" : "(INVALID)");
    printf("  Total slots:  %u\n", hdr->total_slots);
    printf("  Cell size:    %u bytes\n", cell_sz);
    printf("  GGUF type:    %s (%u)\n", type_name(hdr->gguf_type), hdr->gguf_type);
    printf("  Axes:         X=%u Y=%u Z=%u\n", hdr->x_slots, hdr->y_slots, hdr->z_slots);
    printf("  Cube bytes:   %u\n", cube_bytes);
    printf("  CRC-64:       %s\n", computed_crc == stored_crc ? "PASS" : "FAIL");
    printf("  n_elems/capo: %u\n", n_elems_per_capo);
    printf("  Capo:         %u/%u\n", fml->capo_id, capo_total);

    if (computed_crc != stored_crc) {
        fprintf(stderr, "\nERROR: CRC-64 mismatch on capo 0!\n"); free(buf); return 1;
    }

    /* ── DRamTile path: decode into DRamTile slot region ── */
    if (use_dram) {
        DRamTileStore store;
        uint64_t dram_total = (uint64_t)n_elems_per_capo * capo_total;
        dt_store_init(&store, (size_t)dram_total * cell_sz + 4096);

        printf("\n=== DRamTile Decode ===\n");
        uint32_t addr = tess_load_to_dram(tess_path, &store);
        if (addr) {
            printf("  dram_addr = %u (slot = %u)\n", addr, addr % DT_HASH_SLOTS);
            printf("  GPU available: %s\n", tess_scatter_has_cuda() ? "YES" : "NO");
        }

        dt_store_destroy(&store);
        free(buf);
        return addr ? 0 : 1;
    }

    if (!out_path) {
        printf("\n  (info only — pass output path to decode)\n");
        free(buf); return 0;
    }

    /* Total output = capo_total * n_elems_per_capo * cell_size.
     * Last capo may have fewer elements (handled during decode). */
    uint64_t total_elems = (uint64_t)n_elems_per_capo * capo_total;
    uint8_t *out = (uint8_t *)malloc(total_elems * cell_sz);
    if (!out) {         fprintf(stderr, "ERROR: alloc output buffer\n"); free(buf); return 1; }
    memset(out, 0, total_elems * cell_sz);

    /* Decode capo 0 (already loaded in buf) */
    int32_t dec_sz = tess_load_decode(cube_data, cube_bytes, cell_sz, n_elems_per_capo,
                                       out, (uint32_t)(n_elems_per_capo * cell_sz));
    if (dec_sz <= 0) {
        fprintf(stderr, "ERROR: capo 0 decode failed (%d)\n", dec_sz);
        free(out); free(buf); return 1;
    }
    uint64_t total_decoded_elems = n_elems_per_capo;

    /* Load and decode remaining capos */
    for (uint32_t c = 1; c < capo_total; c++) {
        char capo_file[1024];
        capo_path(capo_file, sizeof(capo_file), tess_path, c);

        FILE *cf = fopen(capo_file, "rb");
        if (!cf) {
            fprintf(stderr, "  WARNING: capo %u not found (%s)\n", c, capo_file);
            continue;
        }

        fseek(cf, 0, SEEK_END);
        long capo_sz = ftell(cf);
        fseek(cf, 0, SEEK_SET);

        uint8_t *capo_buf = (uint8_t *)malloc((size_t)capo_sz);
        if (!capo_buf) { fclose(cf); continue; }
        if (fread(capo_buf, 1, (size_t)capo_sz, cf) != (size_t)capo_sz) {
            free(capo_buf); fclose(cf); continue;
        }
        fclose(cf);

        const TESS_Header *chdr = (const TESS_Header *)capo_buf;
        const uint8_t *ccube = capo_buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
        uint32_t ccube_bytes = chdr->total_slots * chdr->cell_size;

        /* Verify CRC */
        uint64_t cc_stored, cc_computed;
        memcpy(&cc_stored, ccube + ccube_bytes, TESS_CRC_SIZE);
        cc_computed = tess_crc64(ccube, ccube_bytes);
        if (cc_computed != cc_stored) {
            fprintf(stderr, "  WARNING: capo %u CRC FAIL\n", c);
            free(capo_buf); continue;
        }

        uint32_t ce = chdr->tensor_count ? chdr->tensor_count : chdr->total_slots;
        uint8_t *dst = out + total_decoded_elems * cell_sz;
        dec_sz = tess_load_decode(ccube, ccube_bytes, chdr->cell_size, ce,
                                   dst, (uint32_t)(ce * chdr->cell_size));
        free(capo_buf);

        if (dec_sz <= 0) {
            fprintf(stderr, "  WARNING: capo %u decode failed (%d)\n", c, dec_sz);
            continue;
        }
        total_decoded_elems += ce;
    }

    uint32_t final_bytes = (uint32_t)(total_decoded_elems * (uint64_t)cell_sz);

    FILE *of = fopen(out_path, "wb");
    if (!of) { fprintf(stderr, "ERROR: cannot write %s\n", out_path); free(out); free(buf); return 1; }
    fwrite(out, 1, (size_t)final_bytes, of);
    fclose(of);

    printf("\n  Decoded: %u capos, %u elements, %u bytes -> %s\n",
           capo_total, (uint32_t)total_decoded_elems, final_bytes, out_path);
    free(out); free(buf); return 0;
}
