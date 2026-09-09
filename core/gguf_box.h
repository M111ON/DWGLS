/* ═══════════════════════════════════════════════════════════════════════════
 * gguf_box.h — GGUF Box: Routing Engine with Mock GGUF Header
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Architecture:
 *   llama.cpp → Our GGUF Box (mock header) → zero copy → real GGUF data
 *
 * The box reads a real GGUF file, builds a lookup table, and generates
 * a synthetic GGUF header that llama.cpp can read. When llama requests
 * tensor data, we return a direct pointer to mmap'd data (zero-copy).
 *
 * This is a PROXY layer — llama.cpp thinks it reads a normal GGUF file.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GGUF_BOX_H
#define GGUF_BOX_H

#include "gguf_reader.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* GNN+Fan24 tile connectivity */
#include "gnn_fan24_model.h"
#include "hyp_fusion.h"

/* ═══════════════════════════════════════════════════════════════════════════
   BOX ENTRY — maps one tensor to its location
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char    *name;      /* tensor name */
    uint32_t       idx;       /* index in GGUF tensor list */
    uint64_t       offset;    /* offset from data section start */
    uint32_t       size;      /* tensor data size in bytes */
    uint8_t        dtype;     /* GGML type code */
    uint32_t       n_dims;    /* number of dimensions */
    uint32_t       dims[4];   /* dimension sizes */
    uint32_t       n_elems;   /* total elements */
    /* Zero-copy pointer: directly into mmap'd file */
    const uint8_t *data;      /* pointer to actual tensor bytes */
} GGUFBoxEntry;

/* ═══════════════════════════════════════════════════════════════════════════
   MOCK GGUF HEADER — synthetic header for llama.cpp
   ═══════════════════════════════════════════════════════════════════════════ */

#define GGUF_MAGIC   0x46554747u
#define GGUF_ALIGN   32u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t n_tensors;
    uint64_t n_kv;
} MockGGUFHeader;
#pragma pack(pop)

/* ═══════════════════════════════════════════════════════════════════════════
   GGUF BOX — the routing engine
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    GgufReader     reader;      /* underlying GGUF reader (mmap'd file) */
    GGUFBoxEntry  *entries;     /* tensor lookup table */
    uint32_t       n_tensors;   /* number of tensors */
    uint64_t       data_offset; /* offset to data section in file */
    int            is_open;     /* 1 if box is ready */

    /* Mock header for llama.cpp */
    uint8_t       *mock_buf;    /* buffer containing mock GGUF file */
    size_t         mock_size;   /* size of mock buffer */
} GGUFBox;

/* ═══════════════════════════════════════════════════════════════════════════
   OPEN — mmap real GGUF + build routing table
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int gguf_box_open(GGUFBox *box, const char *path) {
    memset(box, 0, sizeof(*box));

    /* Open real GGUF file (mmap'd) */
    if (gguf_open(path, &box->reader) != 0) return -1;

    box->n_tensors   = box->reader.n_tensors;
    box->data_offset = box->reader.data_offset;

    /* Build lookup table */
    box->entries = (GGUFBoxEntry *)calloc(box->n_tensors, sizeof(GGUFBoxEntry));
    if (!box->entries) {
        gguf_close(&box->reader);
        return -1;
    }

    for (uint32_t i = 0; i < box->n_tensors; i++) {
        GGUFBoxEntry *e = &box->entries[i];
        e->name   = box->reader.names[i];
        e->idx    = i;
        e->offset = box->reader.offsets[i];
        e->size   = box->reader.sizes[i];
        e->dtype  = box->reader.dtypes[i];
        /* real dims from reader — llama.cpp requires the actual shape */
        e->n_dims = box->reader.n_dims[i];
        e->dims[0] = (uint32_t)box->reader.dims[(size_t)i * 4 + 0];
        e->dims[1] = (uint32_t)box->reader.dims[(size_t)i * 4 + 1];
        e->dims[2] = (uint32_t)box->reader.dims[(size_t)i * 4 + 2];
        e->dims[3] = (uint32_t)box->reader.dims[(size_t)i * 4 + 3];
        e->n_elems = 1;
        for (uint32_t d = 0; d < e->n_dims; d++) e->n_elems *= e->dims[d];

        /* Zero-copy pointer */
        if (box->reader.base &&
            box->data_offset + e->offset + e->size <= box->reader.base_sz) {
            e->data = box->reader.base + box->data_offset + e->offset;
        } else {
            e->data = NULL;
        }
    }

    box->is_open = 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MOCK HEADER — generate synthetic GGUF for llama.cpp
   ═══════════════════════════════════════════════════════════════════════════
 *
 * Creates a buffer that looks like a GGUF file:
 *   [GGUF header] [KV entries] [tensor info] [padding] [data pointer table]
 *
 * The data section is NOT copied — we store pointers to mmap'd data.
 * When llama.cpp reads tensor data, we intercept and return the pointer.
 */

/* Write mock header to buffer — returns buffer and size */
static inline int gguf_box_build_mock(GGUFBox *box,
                                       uint8_t **out_buf,
                                       size_t *out_size)
{
    if (!box->is_open) return -1;

    /* Calculate total size needed:
     *   header: 24 bytes = magic(4) + version(4) + n_tensors(8) + n_kv(8)
     *   KV entries: 0 (we don't need them for routing)
     *   tensor info: n_tensors * (name_len + 4 + dims + dtype + offset)
     *   alignment padding
     */
    size_t hdr_size = 24;  /* MockGGUFHeader — actual written size */
    size_t kv_size = 0;    /* No KV for mock */

    /* Calculate tensor info size */
    size_t tensor_info_size = 0;
    for (uint32_t i = 0; i < box->n_tensors; i++) {
        GGUFBoxEntry *e = &box->entries[i];
        tensor_info_size += 8;                    /* name length */
        tensor_info_size += strlen(e->name);      /* name */
        tensor_info_size += 4;                    /* n_dims */
        tensor_info_size += 4 * 8;               /* dims[4] — int64 each per GGUF spec */
        tensor_info_size += 4;                    /* dtype */
        tensor_info_size += 8;                    /* offset */
    }

    /* Total buffer size */
    size_t total = hdr_size + kv_size + tensor_info_size;
    uint32_t pad = (uint32_t)((GGUF_ALIGN - (total % GGUF_ALIGN)) % GGUF_ALIGN);
    total += pad;

    /* Allocate buffer */
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) return -1;

    /* Write header */
    size_t pos = 0;
    uint32_t magic = GGUF_MAGIC;
    uint32_t version = 3;
    memcpy(buf + pos, &magic, 4); pos += 4;
    memcpy(buf + pos, &version, 4); pos += 4;
    uint64_t nt = box->n_tensors;
    uint64_t nkv = 0;
    memcpy(buf + pos, &nt, 8); pos += 8;
    memcpy(buf + pos, &nkv, 8); pos += 8;

    /* Write tensor info */
    for (uint32_t i = 0; i < box->n_tensors; i++) {
        GGUFBoxEntry *e = &box->entries[i];

        /* Name */
        uint64_t nlen = strlen(e->name);
        memcpy(buf + pos, &nlen, 8); pos += 8;
        memcpy(buf + pos, e->name, nlen); pos += nlen;

        /* n_dims + dims (int64 per GGUF spec) */
        uint32_t nd = e->n_dims;
        memcpy(buf + pos, &nd, 4); pos += 4;
        for (uint32_t d = 0; d < 4; d++) {
            int64_t dv = (int64_t)e->dims[d];
            memcpy(buf + pos, &dv, 8); pos += 8;
        }

        /* dtype */
        memcpy(buf + pos, &e->dtype, 4); pos += 4;

        /* offset */
        memcpy(buf + pos, &e->offset, 8); pos += 8;
    }

    *out_buf = buf;
    *out_size = total;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   FIND — lookup tensor by name
   ═══════════════════════════════════════════════════════════════════════════ */

static inline const GGUFBoxEntry* gguf_box_find(const GGUFBox *box,
                                                 const char *name)
{
    if (!box->is_open) return NULL;
    for (uint32_t i = 0; i < box->n_tensors; i++) {
        if (strcmp(box->entries[i].name, name) == 0)
            return &box->entries[i];
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TENSOR DATA — zero-copy pull
   ═══════════════════════════════════════════════════════════════════════════ */

static inline const uint8_t* gguf_box_data(const GGUFBox *box,
                                            const char *name)
{
    const GGUFBoxEntry *e = gguf_box_find(box, name);
    return e ? e->data : NULL;
}

static inline const uint8_t* gguf_box_data_idx(const GGUFBox *box,
                                                uint32_t idx)
{
    if (!box->is_open || idx >= box->n_tensors) return NULL;
    return box->entries[idx].data;
}

static inline const GGUFBoxEntry* gguf_box_entry(const GGUFBox *box,
                                                  uint32_t idx)
{
    if (!box->is_open || idx >= box->n_tensors) return NULL;
    return &box->entries[idx];
}

/* ═══════════════════════════════════════════════════════════════════════════
   VERIFY — compare mmap pointer vs memcpy
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int gguf_box_verify(const GGUFBox *box) {
    if (!box->is_open) return -1;

    int errors = 0;
    for (uint32_t i = 0; i < box->n_tensors; i++) {
        const GGUFBoxEntry *e = &box->entries[i];
        if (!e->data) {
            printf("  [WARN] tensor '%s' has no data pointer\n", e->name);
            errors++;
            continue;
        }

        uint8_t *buf = (uint8_t *)malloc(e->size);
        if (!buf) { errors++; continue; }

        int rc = gguf_read_tensor(NULL, &box->reader, i, buf, e->size);
        if (rc != 0) {
            printf("  [ERR]  tensor '%s' read failed (rc=%d)\n", e->name, rc);
            errors++;
        } else if (memcmp(buf, e->data, e->size) != 0) {
            printf("  [ERR]  tensor '%s' data mismatch!\n", e->name);
            errors++;
        }
        free(buf);
    }

    return errors;
}

/* ═══════════════════════════════════════════════════════════════════════════
   STATS — print box contents
   ═══════════════════════════════════════════════════════════════════════════ */

static inline void gguf_box_stats(const GGUFBox *box) {
    if (!box->is_open) {
        printf("GGUF Box: not open\n");
        return;
    }

    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < box->n_tensors; i++)
        total_bytes += box->entries[i].size;

    printf("===============================================================\n");
    printf("  GGUF Box — Routing Engine\n");
    printf("---------------------------------------------------------------\n");
    printf("  Tensors:     %u\n", box->n_tensors);
    printf("  Data offset: %lu bytes\n", (unsigned long)box->data_offset);
    printf("  Total data:  %lu bytes (%.1f MB)\n",
           (unsigned long)total_bytes, total_bytes / (1024.0*1024.0));
    printf("  File size:   %lu bytes (%.1f MB)\n",
           (unsigned long)box->reader.base_sz, box->reader.base_sz / (1024.0*1024.0));
    printf("  Zero-copy:   %s\n", box->reader.base ? "YES (mmap'd)" : "NO");
    printf("---------------------------------------------------------------\n");

    uint32_t show = box->n_tensors < 30 ? box->n_tensors : 30;
    for (uint32_t i = 0; i < show; i++) {
        const GGUFBoxEntry *e = &box->entries[i];
        printf("  [%3u] %-45s %8u bytes  %s\n",
               i, e->name, e->size,
               e->data ? "[mmap]" : "[null]");
    }
    if (box->n_tensors > 30)
        printf("  ... (%u more tensors)\n", box->n_tensors - 30);

    printf("===============================================================\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   GNN TENSOR CONNECTIVITY — learned tile routing via gnn_fan24_model.h
   ═══════════════════════════════════════════════════════════════════════════ */

/* Score connectivity between two tensors: probability [0,1].
 * direction: 1.0 = top-bottom, 0.0 = left-right.
 * Uses data offset as proxy for slot position (fast, O(1)). */
static inline float gguf_box_gnn_score(const GGUFBox *box,
                                        uint32_t idx_a, uint32_t idx_b,
                                        float direction)
{
    if (!box->is_open || idx_a >= box->n_tensors || idx_b >= box->n_tensors)
        return 0.0f;
    uint32_t slot_a = (uint32_t)(box->entries[idx_a].offset / 144);
    uint32_t slot_b = (uint32_t)(box->entries[idx_b].offset / 144);
    return hyp_gnn_score_position(slot_a, slot_b);
}

/* ── fp_rank grid (same as Python gnn_colab_train_fan24.py) ── */
static inline void gguf_box_fp_rank_grid(const float *vals, int n, float grid[9]) {
    int idx[81];
    int count = n < 81 ? n : 81;
    for (int i = 0; i < count; i++) idx[i] = i;
    for (int i = 0; i < count - 1; i++) {
        int mx = i;
        for (int j = i + 1; j < count; j++)
            if (vals[idx[j]] > vals[idx[mx]]) mx = j;
        int tmp = idx[i]; idx[i] = idx[mx]; idx[mx] = tmp;
    }
    for (int i = 0; i < 9; i++) {
        int pos = 0;
        for (int j = 0; j < count; j++)
            if (idx[j] == i) { pos = j; break; }
        grid[i] = (float)(pos + 1);
    }
}

/* ── Extract 9 values from tensor data for grid ── */
static inline int gguf_box_extract_grid(const uint8_t *data, uint32_t size,
                                         uint8_t dtype, float grid[9])
{
    int n = 0;
    if (dtype == 8) {
        for (uint32_t off = 2; off + 1 < size && n < 9; off += 34)
            grid[n++] = (float)(int8_t)data[off];
    } else if (dtype == 0) {
        for (uint32_t off = 0; off + 3 < size && n < 9; off += 4) {
            float v; memcpy(&v, data + off, 4);
            grid[n++] = v;
        }
    } else if (dtype == 1) {
        for (uint32_t off = 0; off + 1 < size && n < 9; off += 2) {
            uint16_t h; memcpy(&h, data + off, 2);
            grid[n++] = (float)(int16_t)h / 256.0f;
        }
    }
    return n;
}

/* Full GNN scoring with data features (reads tensor bytes) */
static inline float gguf_box_gnn_score_data(const GGUFBox *box,
                                             uint32_t idx_a, uint32_t idx_b,
                                             float direction)
{
    if (!box->is_open || idx_a >= box->n_tensors || idx_b >= box->n_tensors)
        return 0.0f;
    const GGUFBoxEntry *ea = &box->entries[idx_a];
    const GGUFBoxEntry *eb = &box->entries[idx_b];
    float nf_a[108], ev_a[4], nf_b[108], ev_b[4];
    /* Extract grid → rank → features */
    float raw_a[81], raw_b[81], grid_a[9], grid_b[9];
    int na = gguf_box_extract_grid(ea->data, ea->size, ea->dtype, raw_a);
    int nb = gguf_box_extract_grid(eb->data, eb->size, eb->dtype, raw_b);
    if (na < 9 || nb < 9) return 0.0f;
    gguf_box_fp_rank_grid(raw_a, na, grid_a);
    gguf_box_fp_rank_grid(raw_b, nb, grid_b);
    /* Line sums */
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}
    };
    float lsum_a[8], lsum_b[8];
    for (int l = 0; l < 8; l++) {
        lsum_a[l] = grid_a[lines[l][0]] + grid_a[lines[l][1]] + grid_a[lines[l][2]];
        lsum_b[l] = grid_b[lines[l][0]] + grid_b[lines[l][1]] + grid_b[lines[l][2]];
    }
    gnn_f24_node_features(grid_a, lsum_a, ea->idx, nf_a);
    float ev_tmp[4];
    gnn_f24_edges(lsum_a, ev_tmp);
    memcpy(ev_a, ev_tmp, 16);
    gnn_f24_node_features(grid_b, lsum_b, eb->idx, nf_b);
    gnn_f24_edges(lsum_b, ev_tmp);
    memcpy(ev_b, ev_tmp, 16);
    return gnn_f24_tile_connects(nf_a, ev_a, nf_b, ev_b, direction);
}

/* ── Routing state ── */
typedef struct {
    uint32_t  *rank_order;
    uint32_t   n_ranked;
} GGUFBoxGNN;

/* Greedy routing: for each tensor, pick highest-scored unvisited neighbor */
static inline GGUFBoxGNN gguf_box_build_routing_order(const GGUFBox *box) {
    GGUFBoxGNN gnn = {NULL, 0};
    if (!box->is_open || box->n_tensors < 2) return gnn;
    uint32_t n = box->n_tensors;
    gnn.rank_order = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!gnn.rank_order) return gnn;
    uint8_t *visited = (uint8_t *)calloc(n, 1);
    gnn.rank_order[0] = 0;
    visited[0] = 1;
    gnn.n_ranked = 1;
    for (uint32_t step = 1; step < n; step++) {
        uint32_t prev = gnn.rank_order[step - 1];
        uint32_t best_next = 0;
        float best_score = -1.0f;
        for (uint32_t j = 1; j < n; j++) {
            if (visited[j]) continue;
            float score = gguf_box_gnn_score(box, prev, j, (step & 1) ? 1.0f : 0.0f);
            if (score > best_score) { best_score = score; best_next = j; }
        }
        gnn.rank_order[step] = best_next;
        visited[best_next] = 1;
        gnn.n_ranked = step + 1;
    }
    free(visited);
    return gnn;
}

static inline void gguf_box_gnn_free(GGUFBoxGNN *g) {
    if (g->rank_order) { free(g->rank_order); g->rank_order = NULL; }
    g->n_ranked = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   CLOSE — release box
   ═══════════════════════════════════════════════════════════════════════════ */

static inline void gguf_box_close(GGUFBox *box) {
    if (box->entries) {
        free(box->entries);
        box->entries = NULL;
    }
    if (box->mock_buf) {
        free(box->mock_buf);
        box->mock_buf = NULL;
    }
    gguf_close(&box->reader);
    box->is_open = 0;
}

#endif /* GGUF_BOX_H */
