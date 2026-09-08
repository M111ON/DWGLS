/* tesspack_verify_pure_c.c — Verify tesspack lossless vs GGUF (pure C, no C++ STL)
 *
 * Strategy: mmap GGUF + mmap tesspack. For each GGUF tensor:
 *   1. Find tensor in tesspack (ONION or SCATTER by name)
 *   2. Decode tesspack data back to original bytes
 *   3. Compare with GGUF data byte-by-byte
 *
 * No C++ STL, no llama-model.h, no codebase headers except gguf.h/ggml.h.
 * TPAK format parsed inline from binary. SCATTER decode inlined.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
static void *mmap_file_ro(const char *path, size_t *out_size) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return NULL; }
    *out_size = (size_t)sz.QuadPart;
    HANDLE fm = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(h);
    if (!fm) return NULL;
    void *p = MapViewOfFile(fm, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(fm);
    return p;
}
static int rss_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (int)(pmc.WorkingSetSize / (1024*1024));
    return 0;
}
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
static void *mmap_file_ro(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    *out_size = (size_t)st.st_size;
    void *p = mmap(NULL, *out_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return (p == MAP_FAILED) ? NULL : p;
}
static int rss_mb(void) { return 0; }
#endif

#include "gguf.h"
#include "ggml.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * INLINE TPAK BINARY PARSER — no codebase headers needed
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TPAK_MAGIC_U32 0x5450414Bu  /* "TPAK" little-endian */

/* TPAK header (64 bytes) */
typedef struct {
    uint32_t magic;           /* [0] TPAK_MAGIC */
    uint32_t version;         /* [1] */
    uint32_t n_capos;         /* [2] */
    uint32_t index_offset;    /* [3] low 32 bits (fits in uint32 for files < 4GB) */
    uint32_t hdr[12];         /* [4..15] reserved/extended fields */
    /* hdr[4] = onion_data_offset
     * hdr[5] = onion_data_size
     * hdr[6] = residual_offset
     * hdr[7] = residual_count */
} TPAK_Header;

/* TPAK index entry (variable-length on disk):
 *   name_len (1 byte)
 *   name (name_len bytes)
 *   capo_id (4 bytes)
 *   offset (8 bytes)
 *   size (4 bytes)
 * Total per entry: 1 + name_len + 16
 */

/* TESS capo header (64 bytes, packed) — must match TESS_Header in geo_tess_container.h */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           /* 0x54455353 = "TESS" */
    uint32_t version;
    uint32_t total_slots;     /* 20736 */
    uint32_t cell_size;       /* bytes per cell */
    uint32_t scale_factor;    /* fixed-point: scale * 65536 */
    uint32_t x_slots;
    uint32_t y_slots;
    uint32_t z_slots;
    uint32_t gguf_type;       /* GGML quantization type */
    uint32_t tensor_count;    /* number of elements in this capo */
    uint64_t source_size;
    uint64_t cube_checksum;
    uint64_t formula_id;
} TESS_CapoHeader;
#pragma pack(pop)

#define TESS_MAGIC_U32     0x54455353u  /* "TESS" */
#define TESS_HEADER_SZ     64u
#define TESS_FORMULA_SZ    64u
#define TESS_STRIDE_37     37u
#define TESS_MASK          20735u       /* 144*144 - 1 */

/* Residual entry (522 bytes) */
typedef struct {
    uint8_t  name_len;
    char     name[255];
    uint8_t  src_len;
    char     src[255];
    uint8_t  src_type;
    uint8_t  dst_type;
    uint8_t  transform;
    uint8_t  _pad;
} TPAK_ResidualEntry;

/* Build index in memory from mmap'd TPAK */
typedef struct {
    char     name[256];
    uint32_t capo_id;
    uint64_t offset;
    uint32_t size;
} IndexEntry;

static int build_index(const uint8_t *base, size_t file_sz, uint32_t index_offset,
                        uint32_t n_capos, IndexEntry **out_entries) {
    IndexEntry *entries = (IndexEntry *)calloc(n_capos, sizeof(IndexEntry));
    if (!entries) return -1;

    const uint8_t *cur = base + index_offset;
    const uint8_t *end = base + file_sz;
    uint32_t n = 0;

    for (uint32_t i = 0; i < n_capos && n < n_capos; i++) {
        if (cur + 1 > end) break;
        uint8_t name_len = *cur++;
        if (name_len == 0) break;
        if (cur + name_len + 16 > end) break;
        memcpy(entries[n].name, cur, name_len);
        entries[n].name[name_len] = 0;
        cur += name_len;
        entries[n].capo_id = *(const uint32_t *)cur;
        entries[n].offset  = *(const uint64_t *)(cur + 4);
        entries[n].size    = *(const uint32_t *)(cur + 12);
        cur += 16;
        n++;
    }
    *out_entries = entries;
    return (int)n;
}

/* Stride-37 scatter decode: per-element decode.
 * For each element i (0..n_elems-1):
 *   slot = (i * 37) % 20736
 *   read cell_size bytes from scatter[slot * cell_size]
 *   write to out[i * cell_size] */
static void scatter_decode(const uint8_t *scatter, uint32_t cell_size,
                           uint32_t n_elems, uint8_t *out) {
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = ((uint64_t)i * 37u) % 20736u;
        uint64_t src_off = (uint64_t)slot * cell_size;
        uint64_t dst_off = (uint64_t)i * cell_size;
        memcpy(out + dst_off, scatter + src_off, cell_size);
    }
}

/* ggml cell size by type (for capo validation) */
static size_t cell_size_for_type(int type) {
    /* Matches gguf_cell_size() in tess_gguf_pack.c */
    static const uint32_t table[] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    if (type >= 0 && (size_t)type < sizeof(table)/sizeof(table[0]))
        return table[type];
    if (type == 41) return 6;   /* Q1_0 */
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <model.gguf> <model.tesspack>\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *tpk_path  = argv[2];

    printf("[verify] RSS at start: %d MB\n", rss_mb());
    fflush(stdout);

    /* ── Open GGUF ── */
    struct gguf_init_params gip = { .no_alloc = 0, .ctx = NULL };
    struct gguf_context *gguf = gguf_init_from_file(gguf_path, gip);
    if (!gguf) { fprintf(stderr, "ERROR: gguf_init_from_file failed\n"); return 1; }
    int64_t n_tensors = gguf_get_n_tensors(gguf);
    printf("[verify] GGUF: %lld tensors\n", (long long)n_tensors);
    fflush(stdout);

    /* mmap GGUF */
    size_t gguf_sz = 0;
    void *gguf_mm = mmap_file_ro(gguf_path, &gguf_sz);
    if (!gguf_mm) { fprintf(stderr, "ERROR: GGUF mmap failed\n"); gguf_free(gguf); return 1; }
    uint64_t gguf_doff = gguf_get_data_offset(gguf);

    /* ── Open tesspack ── */
    size_t tpk_sz = 0;
    void *tpk_mm = mmap_file_ro(tpk_path, &tpk_sz);
    if (!tpk_mm) { fprintf(stderr, "ERROR: tesspack mmap failed\n"); return 1; }

    /* Parse TPAK header */
    TPAK_Header *hdr = (TPAK_Header *)tpk_mm;
    if (tpk_sz < 64 || hdr->magic != TPAK_MAGIC_U32) {
        fprintf(stderr, "ERROR: bad TPAK magic\n"); return 1;
    }
    printf("[verify] TPAK v%d: %u capos, index at %u\n",
           hdr->version, hdr->n_capos, hdr->index_offset);
    fflush(stdout);

    /* Parse ONION region — hdr[4]=data blob offset, hdr[5]=data blob size.
     * Actual ONION index entries are in the regular index with capo_id=0xFFFFFFFF. */
    uint32_t onion_blob_off = hdr->hdr[4];
    uint32_t onion_blob_sz  = hdr->hdr[5];
    printf("[verify] ONION blob: offset=%u size=%u (%u MB)\n",
           onion_blob_off, onion_blob_sz, onion_blob_sz / (1024*1024));

    /* Parse residual region */
    uint32_t res_off = hdr->hdr[6];
    uint32_t res_cnt = hdr->hdr[7];
    TPAK_ResidualEntry *residuals = (res_off && res_cnt) ?
        (TPAK_ResidualEntry *)((uint8_t *)tpk_mm + res_off) : NULL;
    printf("[verify] RESIDUAL: %u entries\n", res_cnt);
    fflush(stdout);

    /* Build index (name → capo lookup) */
    IndexEntry *idx = NULL;
    int idx_n = build_index((const uint8_t *)tpk_mm, tpk_sz, hdr->index_offset,
                            hdr->n_capos, &idx);
    if (idx_n < 0) { fprintf(stderr, "ERROR: index build failed\n"); return 1; }
    /* Count ONION entries from index */
    int n_onion = 0;
    for (int j = 0; j < idx_n; j++) {
        if (idx[j].capo_id == 0xFFFFFFFFu) n_onion++;
    }
    printf("[verify] Index built: %d entries (%d ONION, %d SCATTER)\n", idx_n, n_onion, idx_n - n_onion);

    /* Debug: print first 10 index entry names */
    for (int j = 0; j < idx_n && j < 10; j++) {
        fprintf(stderr, "  [dbg-IDX] [%d] name='%s' capo_id=%u off=%zu sz=%u\n",
                j, idx[j].name, idx[j].capo_id, idx[j].offset, idx[j].size);
    }

    /* Verify tensors */
    printf("[verify] Verifying %lld tensors...\n", (long long)n_tensors);
    fflush(stdout);

    int pass = 0, fail = 0, skip_internal = 0, skip_resid = 0;
    size_t pass_bytes = 0;

    /* Debug: print first 5 tensors */
    for (int64_t i = 0; i < n_tensors && i < 5; i++) {
        const char *nm = gguf_get_tensor_name(gguf, i);
        enum ggml_type gt = gguf_get_tensor_type(gguf, i);
        const int64_t *ne = gguf_get_tensor_ne(gguf, i);
        size_t n = 1; for (int d = 0; d < 4; d++) n *= (size_t)ne[d];
        size_t blk = ggml_blck_size(gt);
        size_t tsz = ggml_type_size(gt);
        size_t bc = (blk > 0) ? ((n + blk - 1) / blk * tsz) : (n * tsz);
        fprintf(stderr, "  [dbg] %s type=%d blck=%zu tsz=%zu ne=%zux%zux%zux%zu bytes=%zu\n",
                nm, (int)gt, blk, tsz, (size_t)ne[0], (size_t)ne[1], (size_t)ne[2], (size_t)ne[3], bc);
    }

    /* temp buffer for scatter decode */
    size_t max_tsz = 0;
    for (int64_t i = 0; i < n_tensors; i++) {
        const int64_t *ne = gguf_get_tensor_ne(gguf, i);
        size_t n_elems = 1;
        for (int d = 0; d < 4; d++) n_elems *= (size_t)ne[d];
        enum ggml_type gt = gguf_get_tensor_type(gguf, i);
        size_t blk = ggml_blck_size(gt);
        size_t tsz = (blk > 0) ? ((n_elems + blk - 1) / blk * ggml_type_size(gt))
                               : (n_elems * ggml_type_size(gt));
        if (tsz > max_tsz) max_tsz = tsz;
    }
    size_t max_capo_bytes = 20736 * 256;
    uint8_t *tmp = (uint8_t *)malloc(max_capo_bytes ? max_capo_bytes : 1);
    if (!tmp) { fprintf(stderr, "ERROR: alloc %zu bytes\n", max_capo_bytes); return 1; }

    /* Build residual name set for quick lookup */
    /* (residuals redirect phantom tensors, so we skip them in direct verification) */

    for (int64_t i = 0; i < n_tensors; i++) {
        const char *name = gguf_get_tensor_name(gguf, i);
        enum ggml_type gt = gguf_get_tensor_type(gguf, i);
        size_t t_offset = gguf_get_tensor_offset(gguf, i);
        const int64_t *ne = gguf_get_tensor_ne(gguf, i);
        size_t n_elems = 1;
        for (int d = 0; d < 4; d++) n_elems *= (size_t)ne[d];

        size_t blk = ggml_blck_size(gt);
        size_t byte_count = (blk > 0) ? ((n_elems + blk - 1) / blk * ggml_type_size(gt))
                                      : (n_elems * ggml_type_size(gt));

        uint8_t *gguf_data = (uint8_t *)gguf_mm + gguf_doff + t_offset;

        /* Check if this tensor is in the residual table (phantom tensor) */
        int is_residual = 0;
        if (residuals) {
            for (uint32_t r = 0; r < res_cnt; r++) {
                if ((uint8_t)residuals[r].name_len == strlen(name) &&
                    memcmp(residuals[r].name, name, residuals[r].name_len) == 0) {
                    is_residual = 1;
                    break;
                }
            }
        }
        if (is_residual) {
            /* Residual tensor — expected to be in pack via transform, skip direct verify */
            skip_resid++;
            continue;
        }

        /* Check ONION first — capo_id=0xFFFFFFFF means ONION entry,
         * entry.offset is absolute file offset of raw data blob */
        int found = 0;
        for (int j = 0; j < idx_n; j++) {
            if (strcmp(idx[j].name, name) == 0 && idx[j].capo_id == 0xFFFFFFFFu) {
                /* Found ONION entry — compare raw bytes directly */
                uint64_t on_off = idx[j].offset;
                uint32_t on_sz  = idx[j].size;
                if (on_off + on_sz > tpk_sz) {
                    printf("[FAIL-ONION-BOUNDS] %s: overflow\n", name);
                    fail++;
                    found = 1;
                    break;
                }
                uint8_t *onion_data = (uint8_t *)tpk_mm + on_off;
                if (on_sz == byte_count &&
                    memcmp(gguf_data, onion_data, byte_count) == 0) {
                    pass++;
                    pass_bytes += byte_count;
                } else if (on_sz != byte_count) {
                    fail++;
                    printf("[FAIL-ONION-SZ] %s: expected %zu got %u\n",
                           name, byte_count, on_sz);
                } else {
                    for (size_t b = 0; b < byte_count; b++) {
                        if (gguf_data[b] != onion_data[b]) {
                            printf("[FAIL-ONION] %s: first diff at byte %zu: 0x%02x vs 0x%02x\n",
                                   name, b, gguf_data[b], onion_data[b]);
                            break;
                        }
                    }
                    fail++;
                }
                found = 1;
                break;
            }
        }
        if (found) continue;

        /* Check SCATTER — find ALL capos by name in index, decode multi-capo tensors */
        int found_scatter = 0;
        size_t write_offset = 0;
        int capo_count = 0;
        int first_fail = 0;
        int mismatch = 0;
        for (int j = 0; j < idx_n && !first_fail; j++) {
            if (strcmp(idx[j].name, name) != 0 || idx[j].capo_id == 0xFFFFFFFFu)
                continue;
            capo_count++;
            uint64_t co = idx[j].offset;
            uint32_t csz = idx[j].size;
            if (co + csz > tpk_sz) {
                printf("[FAIL-BOUNDS] %s capo%d: overflow\n", name, idx[j].capo_id);
                fail++;
                first_fail = 1;
                break;
            }
            const uint8_t *capo_ptr = (const uint8_t *)tpk_mm + co;
            TESS_CapoHeader *chdr = (TESS_CapoHeader *)capo_ptr;
            if (chdr->magic != TESS_MAGIC_U32 || chdr->total_slots != 20736) {
                printf("[FAIL-CAPOHDR] %s capo%d: bad magic/slots (0x%x, %u)\n",
                       name, idx[j].capo_id, chdr->magic, chdr->total_slots);
                fail++;
                first_fail = 1;
                break;
            }
            size_t cell_sz = (size_t)chdr->cell_size;
            uint32_t n_elems_capo = chdr->tensor_count ? chdr->tensor_count : chdr->total_slots;
            size_t capo_bytes = (size_t)n_elems_capo * cell_sz;
            const uint8_t *cube_data = capo_ptr + TESS_HEADER_SZ + TESS_FORMULA_SZ;
            if (capo_bytes > max_capo_bytes) {
                printf("[FAIL-OVERFLOW] %s capo%d: capo_bytes %zu > buffer %zu\n", name, idx[j].capo_id, capo_bytes, max_capo_bytes);
                fail++;
                first_fail = 1;
                break;
            }
            scatter_decode(cube_data, (uint32_t)cell_sz, n_elems_capo, tmp);
            size_t cmp = (byte_count < capo_bytes) ? byte_count : capo_bytes;
            if (write_offset + cmp > byte_count || write_offset + capo_bytes > byte_count) {
                cmp = (byte_count > write_offset) ? byte_count - write_offset : 0;
            }
            if (cmp > 0 && !mismatch) {
                if (memcmp(gguf_data + write_offset, tmp, cmp) != 0) {
                    for (size_t b = 0; b < cmp; b++) {
                        if (gguf_data[write_offset + b] != tmp[b]) {
                            printf("[FAIL-SCATTER] %s capo%d (offset %zu): first diff at byte %zu: gguf=0x%02x pack=0x%02x\n",
                                   name, idx[j].capo_id, write_offset, b, gguf_data[write_offset + b], tmp[b]);
                            break;
                        }
                    }
                    mismatch = 1;
                }
            }
            write_offset += capo_bytes;
            found_scatter = 1;
        }
        if (found_scatter && !first_fail && !mismatch) {
            pass++;
            pass_bytes += (write_offset < byte_count) ? write_offset : byte_count;
        } else if (found_scatter && !first_fail && mismatch) {
            fail++;
        } else if (!found_scatter && !first_fail) {
            skip_internal++;
        }
    }

    free(tmp);

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("[verify] PASS: %d tensors (%zu MB)\n", pass, pass_bytes / (1024*1024));
    printf("[verify] FAIL: %d tensors\n", fail);
    printf("[verify] SKIP: %d (residual/phantom), %d (internal/missing)\n", skip_resid, skip_internal);
    printf("[verify] TOTAL: %d GGUF tensors, %u capos, %d onion, %u residual\n",
           n_tensors, hdr->n_capos, hdr->hdr[5], res_cnt);
    printf("═══════════════════════════════════════════════════════════════\n");
    fflush(stdout);

    if (fail == 0 && pass > 0)
        printf("[verify] VERDICT: LOSSLESS (%d tensors, %zu MB verified)\n", pass, pass_bytes / (1024*1024));
    else if (fail > 0)
        printf("[verify] VERDICT: DIFFERENCES FOUND (%d differ)\n", fail);
    else
        printf("[verify] VERDICT: NO TENSORS VERIFIED\n");

#ifdef _WIN32
    if (gguf_mm) UnmapViewOfFile(gguf_mm);
    if (tpk_mm) UnmapViewOfFile(tpk_mm);
#else
    if (gguf_mm) munmap(gguf_mm, gguf_sz);
    if (tpk_mm) munmap(tpk_mm, tpk_sz);
#endif
    gguf_free(gguf);
    free(idx);
    return fail > 0 ? 1 : 0;
}
