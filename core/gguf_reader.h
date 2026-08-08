#ifndef GGUF_READER_H
#define GGUF_READER_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifndef _fseeki64
#define _fseeki64(f,o,w) fseeko(f,(off_t)(o),w)
#endif
#ifndef __int64
#define __int64 long long
#endif
#endif

#define GGUF_MAGIC   0x46554747u
#define GGUF_ALIGN   32u

/* =========================================================
 * GGUF reader — bulk (memory-mapped) implementation
 *
 * The old per-KV fseek() loop was O(n_freads) — Kokoro phonemizer
 * arrays have ~1M strings → 5.3s per open. Bulk mode maps the file
 * once and parses with pointer arithmetic (0 syscalls during parse).
 * ======================================================== */

#if defined(_WIN32)
#include <windows.h>
#endif

typedef struct {
    uint32_t n_tensors;
    uint64_t data_offset;
    uint64_t *offsets;
    uint32_t *sizes;
    char   **names;
    /* bulk mode: whole file mapped into memory (NULL if not) */
    uint8_t *base;
    size_t   base_sz;
#if defined(_WIN32)
    HANDLE   hfile;
    HANDLE   hmap;
#endif
} GgufReader;

/* -- bounded pointer reader helpers --------------------- */
typedef struct { const uint8_t *p; const uint8_t *end; } GBuf;

static inline int gbuf_u32(GBuf *b, uint32_t *out) {
    if ((size_t)(b->end - b->p) < 4) return -1;
    memcpy(out, b->p, 4); b->p += 4;
    return 0;
}
static inline int gbuf_u64(GBuf *b, uint64_t *out) {
    if ((size_t)(b->end - b->p) < 8) return -1;
    memcpy(out, b->p, 8); b->p += 8;
    return 0;
}
static inline int gbuf_skip(GBuf *b, uint64_t n) {
    if ((uint64_t)(b->end - b->p) < n) return -1;
    b->p += n;
    return 0;
}
static inline int gbuf_read(GBuf *b, void *dst, uint64_t n) {
    if ((uint64_t)(b->end - b->p) < n) return -1;
    memcpy(dst, b->p, (size_t)n); b->p += n;
    return 0;
}

/* map file into memory (Windows mmap; fallback malloc+fread) */
static inline int gguf_map_file(const char *path, GgufReader *r) {
#if defined(_WIN32)
    r->hfile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (r->hfile == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER fsz;
    if (!GetFileSizeEx(r->hfile, &fsz) || fsz.QuadPart <= 0) {
        CloseHandle(r->hfile); r->hfile = NULL; return -1;
    }
    r->hmap = CreateFileMappingA(r->hfile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!r->hmap) { CloseHandle(r->hfile); r->hfile = NULL; return -1; }
    r->base = (uint8_t*)MapViewOfFile(r->hmap, FILE_MAP_READ, 0, 0, 0);
    if (!r->base) { CloseHandle(r->hmap); CloseHandle(r->hfile); r->hmap=NULL; r->hfile=NULL; return -1; }
    r->base_sz = (size_t)fsz.QuadPart;
    return 0;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return -1; }
    r->base = (uint8_t*)mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (r->base == MAP_FAILED) { r->base = NULL; return -1; }
    r->base_sz = (size_t)st.st_size;
    return 0;
#endif
}

static inline int gguf_read_tensor(const char *path, const GgufReader *r,
                                    uint32_t idx, uint8_t *buf, uint32_t cap);
static inline void gguf_close(GgufReader *r);

static inline int gguf_open(const char *path, GgufReader *r) {
    memset(r, 0, sizeof(*r));

    if (gguf_map_file(path, r) != 0) return -1;

    GBuf b;
    b.p = r->base;
    b.end = r->base + r->base_sz;

    uint32_t magic, version;
    uint64_t n_tensors, n_kv;
    if (gbuf_u32(&b, &magic) != 0 || magic != GGUF_MAGIC) goto fail;
    if (gbuf_u32(&b, &version) != 0) goto fail;
    if (gbuf_u64(&b, &n_tensors) != 0) goto fail;
    if (gbuf_u64(&b, &n_kv) != 0) goto fail;

    /* skip KV metadata — pointer arithmetic, instant even for 1M strings */
    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen;
        if (gbuf_u64(&b, &klen) != 0) goto fail;
        if (gbuf_skip(&b, klen) != 0) goto fail;

        uint32_t vtype;
        if (gbuf_u32(&b, &vtype) != 0) goto fail;

        if (vtype == 9) {
            uint32_t arr_type;
            uint64_t narr;
            if (gbuf_u32(&b, &arr_type) != 0) goto fail;
            if (gbuf_u64(&b, &narr) != 0) goto fail;
            static const uint8_t esz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
            if (arr_type == 8) {
                for (uint64_t a = 0; a < narr; a++) {
                    uint64_t slen;
                    if (gbuf_u64(&b, &slen) != 0) goto fail;
                    if (gbuf_skip(&b, slen) != 0) goto fail;
                }
            } else if (arr_type < 13) {
                if (gbuf_skip(&b, (uint64_t)esz[arr_type] * narr) != 0) goto fail;
            }
        } else if (vtype <= 12) {
            static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,8,8,8,8};
            if ((size_t)(b.end - b.p) < (size_t)vsz[vtype]) goto fail;
            if (vtype == 8) {
                uint64_t slen;
                if (gbuf_u64(&b, &slen) != 0) goto fail;
                if (gbuf_skip(&b, slen) != 0) goto fail;
            } else {
                b.p += vsz[vtype];
            }
        } else {
            goto fail;
        }
    }

    /* tensor info immediately after KV metadata — no padding in v3 */
    r->n_tensors = (uint32_t)n_tensors;
    r->names = (char **)calloc(n_tensors, sizeof(char *));
    r->offsets = (uint64_t *)calloc(n_tensors, sizeof(uint64_t));
    r->sizes = (uint32_t *)calloc(n_tensors, sizeof(uint32_t));
    if (!r->names || !r->offsets || !r->sizes) goto fail;

    static const struct { uint16_t tsz; uint16_t blck; } tinfo[31] = {
        {4,   1},   /* GGML_TYPE_F32    = 0  */
        {2,   1},   /* GGML_TYPE_F16    = 1  */
        {18,  32},  /* GGML_TYPE_Q4_0   = 2  */
        {20,  32},  /* GGML_TYPE_Q4_1   = 3  */
        {0,   0},   /* 4 removed */
        {0,   0},   /* 5 removed */
        {22,  32},  /* GGML_TYPE_Q5_0   = 6  */
        {24,  32},  /* GGML_TYPE_Q5_1   = 7  */
        {34,  32},  /* GGML_TYPE_Q8_0   = 8  */
        {36,  32},  /* GGML_TYPE_Q8_1   = 9  */
        {84,  256}, /* GGML_TYPE_Q2_K   = 10 */
        {110, 256}, /* GGML_TYPE_Q3_K   = 11 */
        {144, 256}, /* GGML_TYPE_Q4_K   = 12 */
        {176, 256}, /* GGML_TYPE_Q5_K   = 13 */
        {210, 256}, /* GGML_TYPE_Q6_K   = 14 */
        {292, 256}, /* GGML_TYPE_Q8_K   = 15 */
        {2,   256}, /* GGML_TYPE_IQ2_XXS=16 */
        {2,   256}, /* GGML_TYPE_IQ2_XS =17 */
        {2,   256}, /* GGML_TYPE_IQ3_XXS=18 */
        {1,   256}, /* GGML_TYPE_IQ1_S =19 */
        {2,   32},  /* GGML_TYPE_IQ4_NL =20 */
        {1,   256}, /* GGML_TYPE_IQ3_S =21 */
        {1,   256}, /* GGML_TYPE_IQ2_S =22 */
        {2,   256}, /* GGML_TYPE_IQ4_XS =23 */
        {1,   1},   /* GGML_TYPE_I8    =24 */
        {2,   1},   /* GGML_TYPE_I16   =25 */
        {4,   1},   /* GGML_TYPE_I32   =26 */
        {8,   1},   /* GGML_TYPE_I64   =27 */
        {8,   1},   /* GGML_TYPE_F64   =28 */
        {1,   256}, /* GGML_TYPE_IQ1_M =29 */
        {2,   1},   /* GGML_TYPE_BF16  =30 */
    };

    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen;
        if (gbuf_u64(&b, &nlen) != 0) goto fail;
        if (nlen == 0 || nlen > 1024) goto fail;
        char *name = (char *)malloc((size_t)nlen + 1);
        if (!name) goto fail;
        if (gbuf_read(&b, name, nlen) != 0) { free(name); goto fail; }
        name[nlen] = '\0';
        r->names[i] = name;

        uint32_t n_dims;
        if (gbuf_u32(&b, &n_dims) != 0) goto fail;

        int64_t dims[4] = {1,1,1,1};
        for (uint32_t d = 0; d < n_dims; d++) {
            int64_t v;
            if (gbuf_u64(&b, (uint64_t*)&v) != 0) goto fail;
            dims[d] = v;
        }

        uint32_t dtype;
        if (gbuf_u32(&b, &dtype) != 0) goto fail;

        uint64_t data_off;
        if (gbuf_u64(&b, &data_off) != 0) goto fail;

        size_t tsize = 0;
        if (dtype < 31 && tinfo[dtype].tsz > 0 && tinfo[dtype].blck > 0) {
            size_t n_elems = 1;
            for (uint32_t d = 0; d < n_dims; d++) n_elems *= (size_t)dims[d];
            tsize = (n_elems / tinfo[dtype].blck) * tinfo[dtype].tsz;
        }

        r->sizes[i] = (uint32_t)tsize;
        r->offsets[i] = data_off;
    }

    /* data section aligned to 32 */
    size_t pos = (size_t)(b.p - r->base);
    uint32_t pad = (uint32_t)((GGUF_ALIGN - (pos % GGUF_ALIGN)) % GGUF_ALIGN);
    r->data_offset = (uint64_t)pos + pad;
    return 0;

fail:
    gguf_close(r);
    return -1;
}

static inline int gguf_read_tensor(const char *path, const GgufReader *r,
                                    uint32_t idx, uint8_t *buf, uint32_t cap)
{
    if (idx >= r->n_tensors) return -1;
    if (r->sizes[idx] > cap) return -2;

    /* bulk mode: copy straight from mapped memory — zero syscalls */
    if (r->base) {
        uint64_t off = r->data_offset + r->offsets[idx];
        if (off + r->sizes[idx] > r->base_sz) return -4;
        memcpy(buf, r->base + off, r->sizes[idx]);
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return -3;
    if (_fseeki64(f, (__int64)(r->data_offset + r->offsets[idx]), SEEK_SET) != 0) {
        fclose(f); return -4;
    }
    if (fread(buf, r->sizes[idx], 1, f) != 1) { fclose(f); return -5; }
    fclose(f);
    return 0;
}

static inline void gguf_close(GgufReader *r) {
    if (r->names) {
        for (uint32_t i = 0; i < r->n_tensors; i++) free(r->names[i]);
        free(r->names);
    }
    free(r->offsets);
    free(r->sizes);
#if defined(_WIN32)
    if (r->base) UnmapViewOfFile(r->base);
    if (r->hmap) CloseHandle(r->hmap);
    if (r->hfile) CloseHandle(r->hfile);
#else
    if (r->base) munmap(r->base, r->base_sz);
#endif
    memset(r, 0, sizeof(*r));
}

#endif
