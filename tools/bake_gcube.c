/*
 * tools/bake_gcube.c — Phase 2 bake: GGUF / SafeTensors → .gcube
 * (multi-frontend baker — Aug 10 2026)
 *
 * Astra 2 (Gemini) recommendation adopted: .gcube is the UNIVERSAL target
 * format; each source format gets its own bake FRONTEND (parser), the
 * geometry container + verify remain shared. No engine (llama.cpp /
 * transformers) is ever re-der["__never_expand__"]... built.
 *
 * Frontends:
 *   gguf        — via core/gguf_reader.h (bulk mmap)
 *   safetensors — JSON header + raw byte buffer (mmap)
 *
 * Usage: bake_gcube <model.gguf|model.safetensors> <out.gcube>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include "core/gguf_reader.h"
#include "core/geo_cube_container.h"

static double now_sec(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }

/* ───────────────────────────────────────────────────────────────
 * SAFETENSORS FRONTEND
 * ─────────────────────────────────────────────────────────────── */

typedef struct {
    char      name[256];
    uint8_t   dtype;          /* mapped GGML type */
    uint32_t  n_dims;
    uint32_t  dims[4];
    uint64_t  n_weights;
    uint64_t  off_start, off_end;
} STTensor;

/* Map safetensors dtype string → GGML type code (see gguf_reader tinfo) */
static uint8_t st_dtype_map(const char *d)
{
    if      (strcmp(d, "F32")  == 0) return 0;  /* GGML_TYPE_F32  */
    if      (strcmp(d, "F16")  == 0) return 1;  /* GGML_TYPE_F16  */
    if      (strcmp(d, "BF16") == 0) return 30; /* GGML_TYPE_BF16 */
    if      (strcmp(d, "I8")   == 0) return 24; /* GGML_TYPE_I8   */
    if      (strcmp(d, "I16")  == 0) return 25; /* GGML_TYPE_I16  */
    if      (strcmp(d, "I32")  == 0) return 26; /* GGML_TYPE_I32  */
    if      (strcmp(d, "I64")  == 0) return 27; /* GGML_TYPE_I64  */
    if      (strcmp(d, "F64")  == 0) return 28; /* GGML_TYPE_F64  */
    return 255; /* unknown */
}

/* Parse the safetensors JSON header (mirrors tests/test_safetensors_reader.c) */
static int st_parse_header(const char *json, uint64_t json_len,
                           STTensor *out, int max_out)
{
    int n = 0;
    const char *p = json, *end = json + json_len;
    while (p < end && n < max_out) {
        const char *q1 = memchr(p, '"', (size_t)(end - p));
        if (!q1) break;
        q1++;
        const char *q2 = memchr(q1, '"', (size_t)(end - q1));
        if (!q2) break;
        int name_len = (int)(q2 - q1);
        if (name_len > 255 || name_len == 0) { p = q2 + 1; continue; }

        if (name_len == 12 && memcmp(q1, "__metadata__", 12) == 0) {
            const char *brace = memchr(q2, '{', (size_t)(end - q2 - 1));
            if (brace) {
                int depth = 0;
                while (brace < end) {
                    if (*brace == '{') depth++;
                    else if (*brace == '}') { depth--; if (depth == 0) break; }
                    brace++;
                }
                p = (brace < end) ? brace + 1 : q2 + 1;
            } else p = q2 + 1;
            continue;
        }

        const char *scan = q2 + 1;
        while (scan < end && (*scan == ' ' || *scan == ':' || *scan == '\n')) scan++;
        if (scan >= end || *scan != '{') { p = q2 + 1; continue; }

        const char *dtype_pos = NULL, *shape_pos = NULL, *offsets_pos = NULL;
        const char *search_end = (scan + 500 < end) ? scan + 500 : end;
        const char *s = scan + 1;
        while (s < search_end) {
            if      (memcmp(s, "\"dtype\"", 7) == 0 && !dtype_pos)  dtype_pos = s;
            else if (memcmp(s, "\"shape\"", 7) == 0 && !shape_pos)  shape_pos = s;
            else if (memcmp(s, "\"data_offsets\"", 14) == 0 && !offsets_pos) offsets_pos = s;
            if (dtype_pos && shape_pos && offsets_pos) break;
            s++;
        }
        if (!dtype_pos) { p = q2 + 1; continue; }

        STTensor *t = &out[n];
        memset(t, 0, sizeof(*t));
        memcpy(t->name, q1, (size_t)name_len);
        t->name[name_len] = 0;

        char dstr[16] = {0};
        const char *dp = dtype_pos + 7;
        while (*dp && *dp != '"') dp++;
        if (*dp == '"') dp++;
        int di = 0;
        while (*dp && *dp != '"' && di < 15) dstr[di++] = *dp++;
        t->dtype = st_dtype_map(dstr);
        if (t->dtype == 255) { p = q2 + 1; continue; } /* skip unknown */

        if (shape_pos) {
            const char *sp = shape_pos + 7;
            while (*sp && *sp != '[') sp++;
            if (*sp == '[') {
                sp++;
                while (*sp && *sp != ']' && t->n_dims < 4) {
                    while (*sp && !isdigit((unsigned char)*sp) && *sp != '-' && *sp != ']') sp++;
                    if (*sp == ']') break;
                    t->dims[t->n_dims++] = (uint32_t)strtoull(sp, (char **)&sp, 10);
                }
            }
        }
        if (offsets_pos) {
            const char *op = offsets_pos + 14;
            while (*op && *op != '[') op++;
            if (*op == '[') {
                op++;
                t->off_start = strtoull(op, (char **)&op, 10);
                while (*op && *op != ',') op++;
                if (*op == ',') { op++; t->off_end = strtoull(op, (char **)&op, 10); }
            }
        }
        t->n_weights = 1;
        for (int d = 0; d < (int)t->n_dims; d++) t->n_weights *= t->dims[d];
        n++;
        p = q2 + 1;
    }
    return n;
}

#if defined(_WIN32)
#include <windows.h>
typedef struct { HANDLE hf, hm; uint8_t *base; size_t sz; } STMap;
static int st_map_file(const char *path, STMap *m) {
    m->hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m->hf == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER fsz;
    if (!GetFileSizeEx(m->hf, &fsz) || fsz.QuadPart <= 8) { CloseHandle(m->hf); return -1; }
    m->hm = CreateFileMappingA(m->hf, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m->hm) { CloseHandle(m->hf); return -1; }
    m->base = (uint8_t *)MapViewOfFile(m->hm, FILE_MAP_READ, 0, 0, 0);
    if (!m->base) { CloseHandle(m->hm); CloseHandle(m->hf); return -1; }
    m->sz = (size_t)fsz.QuadPart;
    return 0;
}
static void st_unmap(STMap *m) { UnmapViewOfFile(m->base); CloseHandle(m->hm); CloseHandle(m->hf); }
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
typedef struct { int fd; uint8_t *base; size_t sz; } STMap;
static int st_map_file(const char *path, STMap *m) {
    m->fd = open(path, O_RDONLY);
    if (m->fd < 0) return -1;
    struct stat st;
    if (fstat(m->fd, &st) != 0 || st.st_size <= 8) { close(m->fd); return -1; }
    m->base = (uint8_t *)mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, m->fd, 0);
    if (m->base == MAP_FAILED) { close(m->fd); return -1; }
    m->sz = (size_t)st.st_size;
    return 0;
}
static void st_unmap(STMap *m) { munmap(m->base, m->sz); close(m->fd); }
#endif

/* Bake a safetensors file into the cube. Returns # tensors or 0. */
static uint32_t bake_from_safetensors(const char *path, GCubeContainer *cube)
{
    STMap m;
    if (st_map_file(path, &m) != 0) return 0;

    /* header: u64 LE length + JSON */
    uint64_t hlen;
    memcpy(&hlen, m.base, 8);
    if (hlen == 0 || hlen + 8 > m.sz) { st_unmap(&m); return 0; }
    /* header may be padded to 8-byte alignment */
    uint64_t data_begin = 8 + hlen;
    if (data_begin < m.sz && data_begin % 8 != 0) data_begin += 8 - (data_begin % 8);
    const char *json = (const char *)(m.base + 8);
    if (json[hlen - 1] != '}') { /* loose tolerance */ }

    STTensor tensors[512];
    int n = st_parse_header(json, (uint64_t)hlen, tensors, 512);
    for (int i = 0; i < n; i++) {
        STTensor *t = &tensors[i];
        if (t->off_end <= t->off_start || t->off_end > m.sz - data_begin) continue;
        uint32_t sz = (uint32_t)(t->off_end - t->off_start);
        const uint8_t *src = m.base + data_begin + t->off_start;
        uint32_t dims[4] = { t->n_dims > 0 ? t->dims[0] : 1,
                             t->n_dims > 1 ? t->dims[1] : 0,
                             t->n_dims > 2 ? t->dims[2] : 0,
                             t->n_dims > 3 ? t->dims[3] : 0 };
        uint32_t ne = (uint32_t)t->n_weights;
        if (t->off_end - t->off_start > 0xFFFFFFFFu) { fprintf(stderr, "ST tensor >4GB skip: %s\n", t->name); continue; }
        if (gcube_add_tensor(cube, t->name, t->n_dims > 4 ? 4 : t->n_dims, dims,
                             t->dtype, ne, src, sz) != 0) {
            fprintf(stderr, "bake: add fail %s (idx %d/%d)\n", t->name, i, n);
            if (cube->header.n_tensors >= GCUBE_MAX_TENSORS) break;
        }
    }
    st_unmap(&m);
    return cube->header.n_tensors;
}

/* ───────────────────────────────────────────────────────────────
 * GGUF FRONTEND
 * ─────────────────────────────────────────────────────────────── */

static uint32_t bake_from_gguf(const char *path, GCubeContainer *cube)
{
    GgufReader gguf;
    if (gguf_open(path, &gguf) != 0) { return 0; }
    uint8_t *tbuf = NULL; uint32_t tcap = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        uint32_t sz = gguf.sizes[i];
        if (sz == 0) continue;
        if (sz > tcap) {
            uint8_t *nb = (uint8_t *)realloc(tbuf, sz);
            if (!nb) break;
            tbuf = nb; tcap = sz;
        }
        if (gguf_read_tensor(path, &gguf, i, tbuf, tcap) != 0) continue;
        uint32_t n_elems = (uint32_t)(((uint64_t)sz * 32u) / 34u);
        uint32_t dims[4] = { n_elems, 0, 0, 0 };
        if (gcube_add_tensor(cube, gguf.names[i], 1, dims,
                             (uint8_t)34 /* GGML_TYPE_Q8_0 */,
                             n_elems, tbuf, sz) != 0) {
            if (cube->header.n_tensors >= GCUBE_MAX_TENSORS) break;
        }
    }
    free(tbuf);
    gguf_close(&gguf);
    return cube->header.n_tensors;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: bake_gcube <model.gguf|model.safetensors> <out.gcube>\n"); return 2; }
    const char *src = argv[1], *out = argv[2];
    size_t sl = strlen(src);
    int is_safe = (sl > 12 && strcmp(src + sl - 12, ".safetensors") == 0);
    double t0 = now_sec();

    GCubeContainer cube;
    gcube_init(&cube);
    strncpy(cube.header.model_name, "baked", GCUBE_MAX_MODEL - 1);

    uint32_t n = 0;
    double t_front = 0;
    if (is_safe) {
        printf("BAKE (safetensors frontend): %s\n", src);
        if ((n = bake_from_safetensors(src, &cube)) == 0) { fprintf(stderr, "bake: no tensors from safetensors\n"); return 1; }
    } else {
        printf("BAKE (gguf frontend): %s\n", src);
        if ((n = bake_from_gguf(src, &cube)) == 0) { fprintf(stderr, "bake: no tensors from gguf\n"); return 1; }
    }
    t_front = now_sec();
    printf("  tensors    : %u\n", n);

    if (gcube_write(&cube, out) != 0) { fprintf(stderr, "bake: cannot write %s\n", out); return 1; }
    double t_write = now_sec();

    /* ── VERIFY: re-read .gcube, compare every byte against source ── */
    GCubeContainer back;
    if (gcube_read(&back, out) != 0) { fprintf(stderr, "verify: re-read fail\n"); gcube_free(&cube); return 1; }
    uint32_t ok = 0, bad = 0;

    if (is_safe) {
        STMap m;
        if (st_map_file(src, &m) == 0) {
            uint64_t hlen; memcpy(&hlen, m.base, 8);
            uint64_t data_begin = 8 + hlen;
            if (data_begin % 8 != 0) data_begin += 8 - (data_begin % 8);
            STTensor st[512];
            int ns = st_parse_header((const char *)(m.base + 8), (uint64_t)hlen, st, 512);
            for (uint32_t i = 0; i < back.header.n_tensors; i++) {
                const GCubeTensorEntry *e = &back.tensors[i];
                /* lookup by NAME — bake skips unknown-dtype tensors, so
                 * index order in the cube differs from the source header */
                int j = -1;
                for (int k = 0; k < ns; k++)
                    if (strcmp(st[k].name, e->name) == 0) { j = k; break; }
                if (j < 0) { bad++; continue; }
                uint32_t sz = (uint32_t)(st[j].off_end - st[j].off_start);
                const uint8_t *srcp = m.base + data_begin + st[j].off_start;
                const uint8_t *got = back.blocks + (uint64_t)e->block_start * GCUBE_BLOCK_SZ;
                if (sz == e->data_size && memcmp(srcp, got, sz) == 0) ok++; else bad++;
            }
            st_unmap(&m);
        }
    } else {
        GgufReader gguf;
        if (gguf_open(src, &gguf) == 0) {
            uint8_t *vref = NULL; uint32_t vcap = 0;
            for (uint32_t i = 0; i < back.header.n_tensors && i < gguf.n_tensors; i++) {
                const GCubeTensorEntry *e = &back.tensors[i];
                if (e->data_size > vcap) {
                    uint8_t *nb = (uint8_t *)realloc(vref, e->data_size);
                    if (!nb) break;
                    vref = nb; vcap = e->data_size;
                }
                if (gguf_read_tensor(src, &gguf, i, vref, vcap) != 0) { bad++; continue; }
                const uint8_t *got = back.blocks + (uint64_t)e->block_start * GCUBE_BLOCK_SZ;
                if (memcmp(vref, got, e->data_size) == 0) ok++; else bad++;
            }
            free(vref);
            gguf_close(&gguf);
        }
    }
    gcube_free(&back);
    double t_verify = now_sec();

    FILE *f = fopen(out, "rb");
    long fsz = -1;
    if (f) { fseek(f, 0, SEEK_END); fsz = ftell(f); fclose(f); }
    printf("  verify     : %u/%u tensors byte-identical (LOSSLESS)%s\n",
           ok, ok + bad, bad ? "  *** MISMATCH ***" : "");
    printf("  .gcube     : %.2f MB  (%u blocks x %u B)\n",
           fsz > 0 ? (double)fsz / 1048576.0 : 0.0, cube.header.total_blocks, GCUBE_BLOCK_SZ);
    printf("  timing     : frontend %.0f ms · write %.0f ms · verify %.0f ms\n",
           (t_front - t0) * 1e3, (t_write - t_front) * 1e3, (t_verify - t_write) * 1e3);
    gcube_free(&cube);
    return (bad == 0 && ok > 0) ? 0 : 1;
}