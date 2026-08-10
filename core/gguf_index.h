#ifndef GGUF_INDEX_H
#define GGUF_INDEX_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GGUF_MAGIC       0x46554747u
#define GGUF_F32         0u
#define GGUF_F16         1u
#define GGUF_Q8_0        8u

typedef struct {
    uint64_t n_tensors;
    char   **names;
    uint32_t *dtypes;
    uint64_t *offsets;
    uint64_t *sizes;
} GGUFTensorIndex;

/* ── Skip a GGUF value (recursive, type 0-12) ── */
static int skip_gguf_value(FILE *fp, uint32_t type) {
    switch (type) {
        case 0:  case 1:  case 7: {  /* u8, i8, bool — 1 byte */
            uint8_t v; return fread(&v,1,1,fp) ? 0 : -1;
        }
        case 2: case 3: {  /* u16, i16 — 2 bytes */
            uint16_t v; return fread(&v,2,1,fp) ? 0 : -1;
        }
        case 4: case 5: case 6: {  /* u32, i32, f32 — 4 bytes */
            uint32_t v; return fread(&v,4,1,fp) ? 0 : -1;
        }
        case 8: {  /* string */
            uint64_t slen;
            if (fread(&slen, 8, 1, fp) != 1) return -1;
            if (slen > 0 && fseek(fp, (long)slen, SEEK_CUR)) return -1;
            return 0;
        }
        case 9: {  /* array — recursive */
            uint32_t arr_type;
            uint64_t arr_len;
            if (fread(&arr_type, 4, 1, fp) != 1) return -1;
            if (fread(&arr_len, 8, 1, fp) != 1) return -1;
            for (uint64_t i = 0; i < arr_len; i++)
                if (skip_gguf_value(fp, arr_type)) return -1;
            return 0;
        }
        case 10: case 11: case 12: {  /* u64, i64, f64 — 8 bytes */
            uint64_t v; return fread(&v, 8, 1, fp) ? 0 : -1;
        }
        default:
            return -1;
    }
}

/* Read tensor metadata from GGUF file */
static int gguf_idx_open(const char *path, GGUFTensorIndex *idx) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    memset(idx, 0, sizeof(*idx));

    uint32_t magic, version;
    uint64_t n_kv;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1 ||
        fread(&idx->n_tensors, 8, 1, f) != 1 || fread(&n_kv, 8, 1, f) != 1)
        { fclose(f); return -1; }
    if (magic != GGUF_MAGIC) { fclose(f); return -1; }

    /* Skip KV pairs using correct recursive skip function */
    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t klen;
        if (fread(&klen, 8, 1, f) != 1) { fclose(f); return -1; }
        if (klen > 0 && fseek(f, (long)klen, SEEK_CUR)) { fclose(f); return -1; }
        uint32_t vtype;
        if (fread(&vtype, 4, 1, f) != 1) { fclose(f); return -1; }
        if (skip_gguf_value(f, vtype)) { fclose(f); return -1; }
    }

    uint64_t n = idx->n_tensors;
    idx->names   = (char**)calloc(n, sizeof(char*));
    idx->dtypes  = (uint32_t*)calloc(n, sizeof(uint32_t));
    idx->offsets = (uint64_t*)calloc(n, sizeof(uint64_t));
    idx->sizes   = (uint64_t*)calloc(n, sizeof(uint64_t));

    for (uint64_t i = 0; i < n; i++) {
        uint64_t nlen;
        if (fread(&nlen, 8, 1, f) != 1) { n = i; break; }
        idx->names[i] = (char*)calloc(nlen + 1, 1);
        if (nlen > 0) fread(idx->names[i], 1, nlen, f);
        uint32_t n_dims;
        fread(&n_dims, 4, 1, f);
        fseek(f, n_dims * 8, SEEK_CUR);  /* skip dimensions */
        fread(&idx->dtypes[i], 4, 1, f);
        fread(&idx->offsets[i], 8, 1, f);
    }
    idx->n_tensors = n;

    /* Compute tensor sizes from offset gaps (sorted by offset) */
    uint64_t file_end;
    fseek(f, 0, SEEK_END);
    file_end = ftell(f);
    fclose(f);

    for (uint64_t i = 0; i < n; i++) {
        uint64_t ti_off = idx->offsets[i];
        uint64_t next = file_end;
        for (uint64_t j = 0; j < n; j++) {
            if (idx->offsets[j] > ti_off && idx->offsets[j] < next)
                next = idx->offsets[j];
        }
        idx->sizes[i] = next - ti_off;
    }
    return 0;
}

static void gguf_idx_close(GGUFTensorIndex *idx) {
    for (uint64_t i = 0; i < idx->n_tensors; i++)
        free(idx->names[i]);
    free(idx->names);
    free(idx->dtypes);
    free(idx->offsets);
    free(idx->sizes);
    memset(idx, 0, sizeof(*idx));
}

#endif