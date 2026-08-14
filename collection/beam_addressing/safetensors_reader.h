/*
 * safetensors_reader.h — Read HuggingFace safetensors format
 *
 * Format:
 *   [8 bytes] header_length (uint64 LE)
 *   [N bytes] header (JSON tensor metadata)
 *   [M bytes] raw tensor data (BF16/FP16/FP32/etc.)
 *
 * Minimal JSON parser that handles the safetensors format.
 */

#ifndef SAFETENSORS_READER_H
#define SAFETENSORS_READER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* BF16 -> float (simple truncation method) */
static inline float bf16_to_float(uint16_t bf16) {
    uint32_t sign = (uint32_t)(bf16 >> 15) << 31;
    uint32_t exp  = (bf16 >> 7) & 0xFF;
    uint32_t mant = bf16 & 0x7F;
    uint32_t f32;
    if (exp == 0) {
        if (mant == 0) f32 = sign;
        else f32 = sign | ((127 - 126) << 23) | (mant << 16);
    } else if (exp == 0xFF) {
        f32 = sign | 0x7F800000 | (mant << 16);
    } else {
        f32 = sign | (exp << 23) | (mant << 16);
    }
    float result;
    memcpy(&result, &f32, 4);
    return result;
}

/* BF16 -> int8 approximation */
static inline int8_t bf16_to_int8_approx(uint16_t bf16) {
    float f = bf16_to_float(bf16);
    if (f > 127.0f) return 127;
    if (f < -128.0f) return -128;
    return (int8_t)(f + (f >= 0 ? 0.5f : -0.5f));
}

static inline int bf16_sign(uint16_t bf16)    { return (bf16 >> 15) & 1; }
static inline int bf16_exponent(uint16_t bf16) { return (bf16 >> 7) & 0xFF; }
static inline int bf16_mantissa(uint16_t bf16) { return bf16 & 0x7F; }
static inline int bf16_is_zero(uint16_t bf16)   { return (bf16 & 0x7FFF) == 0; }

/* Tensor info */
typedef struct {
    char name[256];
    char dtype[16];
    int  ndim;
    int64_t shape[8];
    uint64_t offset_start;
    uint64_t offset_end;
    uint64_t num_elements;
} STTensor;

/* Safetensors file handle */
typedef struct {
    FILE *fp;
    uint64_t data_offset;
    int num_tensors;
    STTensor *tensors;
} STFile;

/* Parse JSON integer */
static int64_t st_parse_int(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    int64_t val = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
    *pp = p;
    return neg ? -val : val;
}

/* Skip JSON string, return pointer to content (without quotes) */
static const char* st_skip_string(const char *p) {
    while (*p && *p != '"') p++;
    if (*p == '"') p++;
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\') p++; /* skip escaped char */
        p++;
    }
    return start;
}

/* Find next unquoted occurrence of a character */
static const char* st_find_char(const char *p, char c) {
    while (*p) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\') p++;
                p++;
            }
            if (*p == '"') p++;
        } else if (*p == c) {
            return p;
        } else {
            p++;
        }
    }
    return NULL;
}

/* Parse one tensor entry from JSON.
   Returns pointer past the closing }, or NULL on error. */
static const char* st_parse_tensor(const char *p, STTensor *t) {
    memset(t, 0, sizeof(*t));

    /* p should be at the start of the value object (after the colon) */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '{') return NULL;
    p++; /* skip { */

    while (*p && *p != '}') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != '"') break;

        /* Read key */
        const char *key_start = p + 1;
        p++;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        int key_len = (int)(p - key_start);
        p++; /* skip closing " */

        while (*p == ' ' || *p == ':') p++;

        if (key_len == 4 && memcmp(key_start, "dtype", key_len) == 0) {
            /* dtype value: "BF16" */
            while (*p == ' ') p++;
            if (*p == '"') {
                p++;
                const char *dt_start = p;
                while (*p && *p != '"') p++;
                int dt_len = (int)(p - dt_start);
                if (dt_len > 15) dt_len = 15;
                memcpy(t->dtype, dt_start, dt_len);
                t->dtype[dt_len] = '\0';
                p++; /* skip closing " */
            }
        } else if (key_len == 5 && memcmp(key_start, "shape", key_len) == 0) {
            /* shape value: [1, 2, 3] */
            while (*p == ' ') p++;
            if (*p == '[') {
                p++;
                int si = 0;
                while (*p && *p != ']' && si < 8) {
                    while (*p == ' ' || *p == ',') p++;
                    if (*p == '-' || (*p >= '0' && *p <= '9')) {
                        t->shape[si++] = st_parse_int(&p);
                    } else break;
                }
                t->ndim = si;
                if (*p == ']') p++;
            }
        } else if (key_len == 12 && memcmp(key_start, "data_offsets", key_len) == 0) {
            /* data_offsets value: [start, end] */
            while (*p == ' ') p++;
            if (*p == '[') {
                p++;
                while (*p == ' ') p++;
                t->offset_start = (uint64_t)st_parse_int(&p);
                while (*p == ' ' || *p == ',') p++;
                t->offset_end = (uint64_t)st_parse_int(&p);
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
            }
        } else {
            /* Unknown key — skip value */
            while (*p && *p != ',' && *p != '}') p++;
        }

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ',') p++;
    }
    if (*p == '}') p++;

    /* Calculate num_elements */
    int64_t ne = 1;
    for (int i = 0; i < t->ndim; i++) ne *= t->shape[i];
    t->num_elements = (uint64_t)ne;

    return p;
}

/* Open safetensors file and parse header */
static STFile* st_open(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }

    STFile *st = (STFile*)calloc(1, sizeof(STFile));
    st->fp = fp;

    /* Read header length */
    uint64_t hdr_len = 0;
    if (fread(&hdr_len, 8, 1, fp) != 1) { fclose(fp); free(st); return NULL; }
    st->data_offset = 8 + hdr_len;

    /* Read header JSON */
    char *hdr = (char*)malloc(hdr_len + 1);
    if (fread(hdr, 1, hdr_len, fp) != hdr_len) {
        free(hdr); fclose(fp); free(st); return NULL;
    }
    hdr[hdr_len] = '\0';

    /* Count tensor entries by counting "dtype" keys */
    int count = 0;
    {
        const char *s = hdr;
        while ((s = strstr(s, "\"dtype\"")) != NULL) { count++; s++; }
    }
    st->num_tensors = count;
    st->tensors = (STTensor*)calloc(count, sizeof(STTensor));

    /* Parse the top-level JSON object: { "name1": {...}, "name2": {...}, ... } */
    const char *p = hdr;
    while (*p && *p != '{') p++;
    p++; /* skip opening { */

    int idx = 0;
    while (idx < count && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') break;

        /* Read tensor name (key) */
        p++; /* skip opening " */
        const char *name_start = p;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        int name_len = (int)(p - name_start);
        p++; /* skip closing " */

        while (*p == ' ' || *p == ':' || *p == '\t') p++;

        /* Skip __metadata__ key */
        if (name_len == 12 && memcmp(name_start, "__metadata__", 12) == 0) {
            /* Skip value object */
            while (*p && *p != '{') p++;
            if (*p == '{') {
                int depth = 1; p++;
                while (*p && depth > 0) {
                    if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                    if (depth > 0) p++;
                }
                if (*p == '}') p++;
            }
            continue;
        }

        /* Copy name */
        if (name_len > 255) name_len = 255;
        memcpy(st->tensors[idx].name, name_start, name_len);
        st->tensors[idx].name[name_len] = '\0';

        /* Parse value object */
        const char *next = st_parse_tensor(p, &st->tensors[idx]);
        if (!next) break;
        p = next;
        idx++;
    }

    st->num_tensors = idx;
    free(hdr);
    return st;
}

/* Read raw tensor data (caller frees) */
static void* st_read_tensor_data(STFile *st, int idx) {
    STTensor *t = &st->tensors[idx];
    uint64_t size = t->offset_end - t->offset_start;
    void *data = malloc(size);
    if (!data) return NULL;

    fseek(st->fp, (long)(st->data_offset + t->offset_start), SEEK_SET);
    if (fread(data, 1, size, st->fp) != size) {
        free(data);
        return NULL;
    }
    return data;
}

/* Print tensor info */
static void st_print_tensor(const STTensor *t, int idx) {
    printf("  [%2d] %-55s  %s  [", idx, t->name, t->dtype);
    for (int i = 0; i < t->ndim; i++) {
        if (i > 0) printf("x");
        printf("%ld", (long)t->shape[i]);
    }
    printf("]  %lu bytes\n", (unsigned long)(t->offset_end - t->offset_start));
}

/* Close and free */
static void st_close(STFile *st) {
    if (!st) return;
    if (st->fp) fclose(st->fp);
    if (st->tensors) free(st->tensors);
    free(st);
}

#endif /* SAFETENSORS_READER_H */
