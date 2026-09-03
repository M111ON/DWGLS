#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Usage: tess_roundtrip <model.gguf> <tensor_name> <tess_dir>\n"); return 1; }
    const char *gguf_path = argv[1];
    const char *tensor_name = argv[2];
    const char *tess_dir = argv[3];

    GgufReader reader;
    if (gguf_open(gguf_path, &reader) != 0) { fprintf(stderr, "Cannot open GGUF\n"); return 1; }

    int idx = -1;
    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        if (strcmp(reader.names[i], tensor_name) == 0) { idx = (int)i; break; }
    }
    if (idx < 0) { fprintf(stderr, "Tensor '%s' not found\n", tensor_name); gguf_close(&reader); return 1; }

    uint8_t dtype = reader.dtypes[idx];
    uint32_t tsize = reader.sizes[idx];
    uint32_t csize = GGUF_CELL_SIZE[dtype];
    uint32_t n_blocks = tsize / csize;

    fprintf(stderr, "Tensor: %s\n", tensor_name);
    fprintf(stderr, "  Type=%u  blocks=%u  cell=%u  tsize=%u\n", dtype, n_blocks, csize, tsize);

    uint8_t *orig = (uint8_t *)malloc(tsize);
    gguf_read_tensor(gguf_path, &reader, idx, orig, tsize);

    uint32_t n_capos = (n_blocks + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;
    fprintf(stderr, "  capos=%u\n", n_capos);

    uint8_t *decoded = (uint8_t *)malloc((uint64_t)n_blocks * csize);
    memset(decoded, 0, (uint64_t)n_blocks * csize);
    int all_ok = 1;

    for (uint32_t c = 0; c < n_capos; c++) {
        char tess_path[1024];
        if (n_capos == 1)
            snprintf(tess_path, sizeof(tess_path), "%s/%s.tess", tess_dir, tensor_name);
        else
            snprintf(tess_path, sizeof(tess_path), "%s/%s_capo%u.tess", tess_dir, tensor_name, c);

        FILE *f = fopen(tess_path, "rb");
        if (!f) { fprintf(stderr, "  capo %u: Cannot open %s\n", c, tess_path); all_ok = 0; continue; }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = (uint8_t *)malloc((size_t)fsz);
        fread(buf, 1, (size_t)fsz, f);
        fclose(f);

        const TESS_Header *hdr = (const TESS_Header *)buf;
        const uint8_t *cube = buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
        uint32_t cube_bytes = hdr->total_slots * hdr->cell_size;

        uint32_t offset = c * TESS_TOTAL_SLOTS;
        uint32_t chunk = n_blocks - offset;
        if (chunk > TESS_TOTAL_SLOTS) chunk = TESS_TOTAL_SLOTS;

        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t slot = tess_stride_scatter(i);
            if (slot >= TESS_TOTAL_SLOTS) slot = i % TESS_TOTAL_SLOTS;
            uint32_t src_off = slot * csize;
            if (src_off + csize <= cube_bytes)
                memcpy(decoded + (uint64_t)(offset + i) * csize, cube + src_off, csize);
        }
        fprintf(stderr, "  capo %u: %u blocks decoded from %s\n", c, chunk, tess_path);
        free(buf);
    }

    int diff = memcmp(orig, decoded, tsize);
    fprintf(stderr, "  Result: %s\n", diff == 0 ? "BITWISE IDENTICAL" : "DIFFERENT");
    if (diff != 0) {
        for (uint32_t i = 0; i < tsize; i++) {
            if (orig[i] != decoded[i]) {
                fprintf(stderr, "  First diff at byte %u: orig=0x%02x dec=0x%02x\n", i, orig[i], decoded[i]);
                break;
            }
        }
    }
    free(orig); free(decoded); gguf_close(&reader);
    return diff;
}
