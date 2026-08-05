/* test_safetensors_reader.c — φ-microscope on SafeTensors format */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "../core/geo_phi_microscope.h"

typedef struct {
    char     name[256];
    char     dtype[16];
    uint64_t shape[8];
    int      n_dims;
    uint64_t data_offsets[2];
    uint64_t n_weights;
} STTensor;

/* Convert BF16 (16-bit brain float) to F32 */
static float bf16_to_f32(uint16_t v) {
    uint32_t sign = (v >> 15) & 1;
    uint32_t exp  = (v >> 7) & 0xFF;
    uint32_t mant = v & 0x7F;
    uint32_t f32;
    if (exp == 0)       f32 = (sign << 31) | (mant << 16);
    else if (exp == 0xFF) f32 = (sign << 31) | 0x7F800000 | (mant << 16);
    else                 f32 = (sign << 31) | ((exp + 127 - 127) << 23) | (mant << 16);
    float r; memcpy(&r, &f32, 4); return r;
}

/* Parse SafeTensors JSON properly:
 * Format: { "name": {"dtype":"X", "shape":[...], "data_offsets":[start,end]}, ... }
 * We look for pattern: quoted_name followed by { "dtype" ... } */
static int parse_st_header(const char *json, uint64_t json_len, STTensor *out, int max_out) {
    int n = 0;
    const char *p = json;
    const char *end = json + json_len;

    while (p < end && n < max_out) {
        /* Find opening quote of a key */
        const char *q1 = memchr(p, '"', end - p);
        if (!q1) break;
        q1++;
        const char *q2 = memchr(q1, '"', end - q1);
        if (!q2) break;

        /* Extract potential tensor name */
        int name_len = (int)(q2 - q1);
        if (name_len > 255 || name_len == 0) { p = q2 + 1; continue; }

        /* Check if this is __metadata__ — skip it and its value block */
        if (name_len == 12 && memcmp(q1, "__metadata__", 12) == 0) {
            /* Skip to matching closing brace (count braces) */
            const char *brace = memchr(q2, '{', end - (q2+1));
            if (brace) {
                int depth = 0;
                while (brace < end) {
                    if (*brace == '{') depth++;
                    else if (*brace == '}') { depth--; if (depth == 0) break; }
                    brace++;
                }
                if (brace < end) p = brace + 1;
                else p = q2 + 1;
            } else {
                p = q2 + 1;
            }
            continue;
        }

        /* After the name key, expect ": {" then "dtype" */
        const char *scan = q2 + 1;
        while (scan < end && (*scan == ' ' || *scan == ':' || *scan == '\n')) scan++;
        if (scan >= end || *scan != '{') { p = q2 + 1; continue; }

        /* Look for "dtype" within the next 200 chars */
        const char *dtype_pos = NULL;
        const char *shape_pos = NULL;
        const char *offsets_pos = NULL;
        const char *search_end = (scan + 500 < end) ? scan + 500 : end;

        const char *s = scan + 1;
        while (s < search_end) {
            if (memcmp(s, "\"dtype\"", 7) == 0 && !dtype_pos) dtype_pos = s;
            else if (memcmp(s, "\"shape\"", 7) == 0 && !shape_pos) shape_pos = s;
            else if (memcmp(s, "\"data_offsets\"", 14) == 0 && !offsets_pos) offsets_pos = s;
            if (dtype_pos && shape_pos && offsets_pos) break;
            s++;
        }

        if (!dtype_pos) { p = q2 + 1; continue; }

        /* Extract tensor */
        STTensor *t = &out[n];
        memset(t, 0, sizeof(*t));
        memcpy(t->name, q1, name_len);
        t->name[name_len] = 0;

        /* dtype: "dtype": "VALUE" */
        const char *dp = dtype_pos + 7;
        while (*dp && *dp != '"') dp++;
        if (*dp == '"') dp++;
        int di = 0;
        while (*dp && *dp != '"' && di < 15) t->dtype[di++] = *dp++;
        t->dtype[di] = 0;

        /* shape: "shape": [n0, n1, ...] */
        if (shape_pos) {
            const char *sp = shape_pos + 7;
            while (*sp && *sp != '[') sp++;
            if (*sp == '[') {
                sp++;
                t->n_dims = 0;
                while (*sp && *sp != ']' && t->n_dims < 8) {
                    while (*sp && !isdigit(*sp) && *sp != '-' && *sp != ']') sp++;
                    if (*sp == ']') break;
                    t->shape[t->n_dims++] = strtoull(sp, (char**)&sp, 10);
                }
            }
        }

        /* data_offsets: "data_offsets": [start, end] */
        if (offsets_pos) {
            const char *op = offsets_pos + 14;
            while (*op && *op != '[') op++;
            if (*op == '[') {
                op++;
                t->data_offsets[0] = strtoull(op, (char**)&op, 10);
                while (*op && *op != ',') op++;
                if (*op == ',') { op++; t->data_offsets[1] = strtoull(op, (char**)&op, 10); }
            }
        }

        t->n_weights = 1;
        for (int d = 0; d < t->n_dims; d++) t->n_weights *= t->shape[d];

        n++;
        p = q2 + 1;
    }
    return n;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "F:/model/zimage/ae.safetensors";

    printf("=== SafeTensors Reader + phi-Microscope ===\n");
    printf("File: %s\n\n", path);

    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("Cannot open file\n"); return 1; }

    uint64_t header_len;
    if (fread(&header_len, 8, 1, fp) != 1) { printf("Read header_len failed\n"); fclose(fp); return 1; }
    printf("Header length: %lu bytes\n\n", header_len);

    char *header = (char*)malloc(header_len + 1);
    if (!header) { printf("OOM\n"); fclose(fp); return 1; }
    if (fread(header, 1, (size_t)header_len, fp) != (size_t)header_len) {
        printf("Read header failed\n"); free(header); fclose(fp); return 1;
    }
    header[header_len] = 0;

    STTensor tensors[128];
    int n_tensors = parse_st_header(header, header_len, tensors, 128);

    printf("Tensors found: %d\n\n", n_tensors);
    printf("  %-45s %8s %12s\n", "Name", "Dtype", "Weights");
    printf("  %-45s %8s %12s\n", "----", "-----", "-------");
    for (int i = 0; i < n_tensors && i < 30; i++) {
        printf("  %-45s %8s %12lu\n", tensors[i].name, tensors[i].dtype,
               (unsigned long)tensors[i].n_weights);
    }
    printf("\n");

    /* Find largest BF16 tensor */
    int best = -1;
    uint64_t best_size = 0;
    for (int i = 0; i < n_tensors; i++) {
        if (tensors[i].n_weights > best_size &&
            (strcmp(tensors[i].dtype, "BF16") == 0 ||
             strcmp(tensors[i].dtype, "F32") == 0 ||
             strcmp(tensors[i].dtype, "F16") == 0)) {
            best_size = tensors[i].n_weights;
            best = i;
        }
    }

    if (best < 0) {
        printf("No compatible tensor found\n");
        free(header); fclose(fp); return 1;
    }

    STTensor *t = &tensors[best];
    printf("Analyzing: %s (%s, %lu weights)\n", t->name, t->dtype, (unsigned long)t->n_weights);

    uint64_t data_offset = 8 + header_len + t->data_offsets[0];
    uint64_t data_size = t->data_offsets[1] - t->data_offsets[0];

    uint64_t max_floats = 8192;
    uint64_t bytes_per = (strcmp(t->dtype, "BF16") == 0 || strcmp(t->dtype, "F16") == 0) ? 2 : 4;
    uint64_t read_size = max_floats * bytes_per;
    if (read_size > data_size) read_size = data_size;
    if (read_size > 2 * 1024 * 1024) read_size = 2 * 1024 * 1024;

    uint8_t *raw = (uint8_t*)malloc(read_size);
    if (!raw) { printf("OOM\n"); free(header); fclose(fp); return 1; }

    fseek(fp, (long)data_offset, SEEK_SET);
    size_t rd = fread(raw, 1, (size_t)read_size, fp);
    fclose(fp);
    free(header);

    float *weights = (float*)malloc(max_floats * sizeof(float));
    uint64_t n_weights = 0;

    if (strcmp(t->dtype, "BF16") == 0) {
        n_weights = rd / 2;
        if (n_weights > max_floats) n_weights = max_floats;
        for (uint64_t i = 0; i < n_weights; i++) {
            uint16_t v; memcpy(&v, raw + i * 2, 2);
            weights[i] = bf16_to_f32(v);
        }
    } else if (strcmp(t->dtype, "F16") == 0) {
        n_weights = rd / 2;
        if (n_weights > max_floats) n_weights = max_floats;
        for (uint64_t i = 0; i < n_weights; i++) {
            uint16_t v; memcpy(&v, raw + i * 2, 2);
            /* F16 to F32 (same as BF16 but different exp bias) */
            uint32_t sign = (v >> 15) & 1;
            uint32_t exp = (v >> 10) & 0x1F;
            uint32_t mant = v & 0x3FF;
            uint32_t f32;
            if (exp == 0) f32 = (sign << 31) | (mant << 13);
            else if (exp == 31) f32 = (sign << 31) | 0x7F800000 | (mant << 13);
            else f32 = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
            memcpy(&weights[i], &f32, 4);
        }
    } else { /* F32 */
        n_weights = rd / 4;
        if (n_weights > max_floats) n_weights = max_floats;
        for (uint64_t i = 0; i < n_weights; i++) memcpy(&weights[i], raw + i * 4, 4);
    }

    printf("Loaded: %lu float weights\n\n", (unsigned long)n_weights);

    if (n_weights > 0) phi_microscope(weights, n_weights, 0, 8);

    free(raw);
    free(weights);
    printf("\n=== DONE ===\n");
    return 0;
}
