#pragma once
#ifndef KV_COLD_BASE_H
#define KV_COLD_BASE_H
/*
 * core/kv_cold_base.h — COLD-KV step 1: base HOLD (clipboard semantics).
 * ═══════════════════════════════════════════════════════════════════════
 * Arch: docs/COLD-KV-ARCH-2026-09-23.md build order step 1.
 * Port of the FGLS_new kv_snapshot_layer SHAPE (runner/kv_sid_evict.h):
 *   snapshot → restore → free_snapshots, one slot, replace-on-hold.
 * Differences from FGLS_new (deliberate, not drops):
 *   - No ggml tensor->data +248 hack: uses public llama_state_seq_* API
 *     (llama.h), so no struct-layout dependency.
 *   - No binary-shell/zstd codec: base is F16 FULL (source of truth per
 *     arch — "original chat log never deleted", base is re-derivable).
 *     Compression of the base is step-2+ concern (K8V4 delta spill).
 *   - ctx is void* so this header stays std-only (repo rule for core/).
 *     The tool casts to struct llama_context*. Declarations only —
 *     implemented in the TU that includes llama.h (see tools/kv_cold_base.c).
 *
 * Clipboard semantics: exactly ONE base held. hold() replaces any
 * previous base (free + take). clear() empties the clipboard.
 *
 * RECONSTRUCT RULE (step 2): the spill unit is a TOKEN suffix, not bytes —
 * measured 2026-09-23: opaque state blobs churn 98.6% per decoded token
 * (uniform across all 24/24 chunks, deltaA vs deltaB 29→30 tok), so
 * byte-deltas on the blob can never pay. KV cells are append-only per
 * position within one sequence, therefore:
 *   live(t_n) = resume(base@t0) + teacher-decode(tokens[t0..t_n]).
 * Bit-exact iff same model + same kernel (flash DISABLED) + same threads.
 *
 * NOTE: hold()/resume() call llama_state_seq_* — include llama.h BEFORE
 * this header in the tool TU (struct llama_context is only forward-used).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct llama_context;

#define KVCB_MAGIC 0x4243564BU  /* "KVCB" LE: cold-base file */
#define KVCB_VER   1
#define KVD_MAGIC  0x4456424BU  /* "KBVD" LE: token-suffix delta file */
#define KVD_VER    1

typedef struct {
    uint8_t *blob;      /* raw llama_state_seq_get_data bytes (F16 full) */
    size_t   size;      /* blob length */
    int32_t  n_tokens;  /* tokens decoded at hold time (re-anchor counting) */
    int      have;      /* 1 = clipboard holds a base */
} KVColdBase;

typedef struct {
    uint32_t magic;
    uint32_t ver;
    int32_t  n_tokens;
    uint32_t reserved;
    uint64_t size;
} KVCBFileHdr;

static inline void kvcb_init(KVColdBase *b) { memset(b, 0, sizeof(*b)); }

static inline void kvcb_clear(KVColdBase *b) {
    free(b->blob); b->blob = NULL; b->size = 0; b->n_tokens = 0; b->have = 0;
}

/* HOLD (snapshot): replace clipboard with current seq state. */
static inline int kvcb_hold(struct llama_context *ctx, int seq, KVColdBase *b, int32_t n_tokens) {
    size_t sz = llama_state_seq_get_size(ctx, seq);
    if (sz == 0) return -1;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) return -1;
    if (llama_state_seq_get_data(ctx, buf, sz, seq) != sz) { free(buf); return -1; }
    kvcb_clear(b);
    b->blob = buf; b->size = sz; b->n_tokens = n_tokens; b->have = 1;
    return 0;
}

/* RESUME: copy held base back into seq. Returns bytes applied, 0 on fail. */
static inline size_t kvcb_resume(struct llama_context *ctx, int seq, const KVColdBase *b) {
    if (!b->have) return 0;
    return llama_state_seq_set_data(ctx, b->blob, b->size, seq);
}

/* File persist: [16B hdr][blob]. Returns 0 on ok. */
static inline int kvcb_save(const KVColdBase *b, const char *path) {
    if (!b->have || !b->blob || b->size == 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    KVCBFileHdr h;
    h.magic = KVCB_MAGIC; h.ver = KVCB_VER;
    h.n_tokens = b->n_tokens; h.reserved = 0; h.size = (uint64_t)b->size;
    int rc = -1;
    if (fwrite(&h, sizeof(h), 1, f) == 1 &&
        fwrite(b->blob, 1, b->size, f) == b->size) rc = 0;
    fclose(f);
    return rc;
}

/* Load replaces clipboard content. Returns 0 on ok. */
static inline int kvcb_load(KVColdBase *b, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    KVCBFileHdr h;
    int rc = -1;
    if (fread(&h, sizeof(h), 1, f) == 1 &&
        h.magic == KVCB_MAGIC && h.ver == KVCB_VER && h.size > 0 &&
        h.size < (uint64_t)(1ULL << 32)) {
        uint8_t *buf = (uint8_t *)malloc((size_t)h.size);
        if (buf && fread(buf, 1, (size_t)h.size, f) == (size_t)h.size) {
            kvcb_clear(b);
            b->blob = buf; b->size = (size_t)h.size;
            b->n_tokens = h.n_tokens; b->have = 1;
            rc = 0;
        } else free(buf);
    }
    fclose(f);
    return rc;
}

/* ── Step 2: token-suffix delta spill ──
 * File: [KVDFileHdr][n_delta × int32 token]. llama_token is int32; the
 * header stays llama-free by using int32_t (caller casts). */

typedef struct {
    uint32_t magic;
    uint32_t ver;
    int32_t  base_n_tokens;  /* tokens covered by the base this deltas from */
    uint32_t n_delta;        /* suffix token count */
} KVDFileHdr;

static inline int kvcb_spill(const int32_t *dtoks, uint32_t n_delta,
                             int32_t base_n_tokens, const char *path) {
    if (!dtoks || n_delta == 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    KVDFileHdr h;
    h.magic = KVD_MAGIC; h.ver = KVD_VER;
    h.base_n_tokens = base_n_tokens; h.n_delta = n_delta;
    int rc = -1;
    if (fwrite(&h, sizeof(h), 1, f) == 1 &&
        fwrite(dtoks, sizeof(int32_t), n_delta, f) == n_delta) rc = 0;
    fclose(f);
    return rc;
}

/* Unspill: mallocs token array into *out_dtoks (caller frees). */
static inline int kvcb_unspill(int32_t **out_dtoks, uint32_t *out_n,
                               int32_t *out_base_n, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    KVDFileHdr h;
    int rc = -1;
    if (fread(&h, sizeof(h), 1, f) == 1 &&
        h.magic == KVD_MAGIC && h.ver == KVD_VER && h.n_delta > 0 &&
        h.n_delta < (1u << 24)) {
        int32_t *t = (int32_t *)malloc((size_t)h.n_delta * sizeof(int32_t));
        if (t && fread(t, sizeof(int32_t), h.n_delta, f) == h.n_delta) {
            *out_dtoks = t; *out_n = h.n_delta; *out_base_n = h.base_n_tokens;
            rc = 0;
        } else free(t);
    }
    fclose(f);
    return rc;
}

#endif /* KV_COLD_BASE_H */
